/**
 * @file
 * @brief Functions related to special abilities.
**/

#include "AppHdr.h"

#include "ability.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>

#include "abyss.h"
#include "areas.h"
#include "art-enum.h"
#include "attack.h" // Attack Strength Punctuation
#include "branch.h"
#include "chardump.h"
#include "cleansing-flame-source-type.h"
#include "cloud.h"
#include "coordit.h"
#include "database.h"
#include "decks.h"
#include "delay.h"
#include "describe.h"
#include "directn.h"
#include "dungeon.h"
#include "evoke.h"
#include "exercise.h"
#include "fight.h"
#include "food.h"
#include "god-abil.h"
#include "god-companions.h"
#include "god-conduct.h"
#include "god-passive.h"
#include "hints.h"
#include "invent.h"
#include "item-prop.h"
#include "items.h"
#include "item-use.h"
#include "level-state-type.h"
#include "libutil.h"
#include "macro.h"
#include "mapmark.h"
#include "maps.h"
#include "menu.h"
#include "message.h"
#include "mon-place.h"
#include "mutation.h"
#include "notes.h"
#include "options.h"
#include "output.h"
#include "player-stats.h"
#include "potion.h"
#include "prompt.h"
#include "religion.h"
#include "skills.h"
#include "spl-cast.h"
#include "spl-clouds.h"
#include "spl-damage.h"
#include "spl-goditem.h"
#include "spl-miscast.h"
#include "spl-other.h"
#include "spl-selfench.h"
#include "spl-summoning.h"
#include "spl-transloc.h"
#include "stairs.h"
#include "state.h"
#include "stepdown.h"
#include "stringutil.h"
#include "target.h"
#include "terrain.h"
#include "tilepick.h"
#include "transform.h"
#include "traps.h"
#include "uncancel.h"
#include "unicode.h"
#include "view.h"
#include "viewchar.h"

#ifdef USE_TILE
# include "rltiles/tiledef-icons.h"
#endif

enum class abflag
{
    none                = 0x00000000,
    breath              = 0x00000001, // ability uses DUR_BREATH_WEAPON
    delay               = 0x00000002, // ability has its own delay
    pain                = 0x00000004, // ability must hurt player (ie torment)
    piety               = 0x00000008, // ability has its own piety cost
    exhaustion          = 0x00000010, // fails if you.exhausted
    instant             = 0x00000020, // doesn't take time to use
    berserk_only        = 0x00000040, // can only be used while berserk
    silence_ok          = 0x00000080, // can be used while silenced
    conf_ok             = 0x00000100, // can use even if confused
                        //0x00000200, // was rations
                        //0x00000400, // was rations_or_piety
                        //0x00000800, // was variable_mp
                        //0x00001000,
                        //0x00002000,
                        //0x00004000,
                        //0x00008000,
                        //0x00010000,
                        //0x00020000,
                        //0x00040000, // was remove_curse_scroll
    skill_drain         = 0x00080000, // drains skill levels
    gold                = 0x00100000, // costs gold
    sacrifice           = 0x00200000, // sacrifice (Ru)
    hostile             = 0x00400000, // failure summons a hostile (Makhleb)
    starve_ok           = 0x00800000, // can use even if starving
    berserk_ok          = 0x01000000, // can use even if berserk
};
DEF_BITFIELD(ability_flags, abflag);

struct scaling_cost
{
    int value;

    scaling_cost(int permille) : value(permille) {}

    static scaling_cost fixed(int fixed)
    {
        return scaling_cost(-fixed);
    }

    int cost(int max) const;

    operator bool () const { return value != 0; }
};

/// What affects the failure chance of the ability?
enum class fail_basis
{
    xl,
    evo,
    invo,
    spider, // spellpower-based fail chance for spider mount.
};

/**
 * What skill is used to determine the player's god's invocations' failure
 * chance?
 *
 * XXX: deduplicate this with the similar code for divine titles, etc
 * (skills.cc:skill_title_by_rank)
 *
 * IMPORTANT NOTE: functions that depend on this will be wrong if you aren't
 * currently worshipping a god that grants the given ability (e.g. in ?/A)!
 *
 * @return      The appropriate skill type; e.g. SK_INVOCATIONS.
 */
skill_type invo_skill(god_type god)
{
    switch (god)
    {
        case GOD_KIKUBAAQUDGHA:
            return SK_NECROMANCY;

#if TAG_MAJOR_VERSION == 34
        case GOD_PAKELLAS:
            return SK_EVOCATIONS;
#endif
        case GOD_ASHENZARI:
        case GOD_GOZAG:
        case GOD_RU:
        case GOD_TROG:
        case GOD_WU_JIAN:
            return SK_NONE;
        default:
            return SK_INVOCATIONS;
    }
}

/// How to determine the odds of the ability failing?
struct failure_info
{
    /// what determines the variable portion of failure: e.g. xl, evo, invo
    fail_basis basis;
    /// base failure chance
    int base_chance;
    /// multiplier to skill/xl; subtracted from base fail chance
    int variable_fail_mult;
    /// denominator to piety; subtracted from base fail chance if invo
    int piety_fail_denom;

    /**
     * What's the chance of the ability failing if the player tries to use it
     * right now?
     *
     * See spl-cast.cc:_get_true_fail_rate() for details on what this 'chance'
     * actually means.
     *
     * @return  A failure chance; may be outside the 0-100 range.
     */
    int chance() const
    {
        switch (basis)
        {
        case fail_basis::xl:
            return base_chance - you.experience_level * variable_fail_mult;
        case fail_basis::evo:
            return base_chance - you.skill(SK_EVOCATIONS, variable_fail_mult);
        case fail_basis::invo:
        {
            const int sk_mod = invo_skill() == SK_NONE ? 0 :
                                 you.skill(invo_skill(), variable_fail_mult);
            const int piety_mod
                = piety_fail_denom ? you.piety / piety_fail_denom : 0;
            return base_chance - sk_mod - piety_mod;
        }
        case fail_basis::spider:
            return base_chance - (calc_spell_power(SPELL_SUMMON_SPIDER_MOUNT, true) / 10 * variable_fail_mult);
        default:
            die("unknown failure basis %d!", (int)basis);
        }
    }

    /// What skill governs the use of this ability, if any?
    skill_type skill() const
    {
        switch (basis)
        {
        case fail_basis::evo:
            return SK_EVOCATIONS;
        case fail_basis::invo:
            return invo_skill();
        case fail_basis::xl:
        default:
            return SK_NONE;
        }
    }
};

// Structure for representing an ability:
struct ability_def
{
    ability_type        ability;
    const char *        name;
    unsigned int        mp_cost;        // magic cost of ability
    scaling_cost        hp_cost;        // hit point cost of ability
    unsigned int        food_cost;      // + rand2avg(food_cost, 2)
    unsigned int        piety_cost;     // piety cost of ability
    failure_info        failure;        // calculator for failure odds
    ability_flags       flags;          // used for additional cost notices
};

static int _lookup_ability_slot(ability_type abil);
static spret _do_ability(const ability_def& abil, bool fail, bool empowered = false);
static void _pay_ability_costs(const ability_def& abil);

// The description screen was way out of date with the actual costs.
// This table puts all the information in one place... -- bwr
//
// The four numerical fields are: MP, HP, food, and piety.
// Note:  food_cost  = val + random2avg(val, 2)
//        piety_cost = val + random2((val + 1) / 2 + 1);
//        hp cost is in per-mil of maxhp (i.e. 20 = 2% of hp, rounded up)
static const ability_def Ability_List[] =
{
    // NON_ABILITY should always come first
    { ABIL_NON_ABILITY, "No ability", 0, 0, 0, 0, {}, abflag::none },
    { ABIL_SPIT_POISON, "Spit Poison",
        0, 0, 80, 0, {fail_basis::xl, 20, 1}, abflag::breath },

    { ABIL_BLINK, "Blink", 0, 90, 50, 0, {fail_basis::xl, -1}, abflag::none },
    // ^ failure special-cased

    { ABIL_BREATHE_FIRE, "Breathe Fire",
      0, 0, 250, 0, {fail_basis::xl, 30, 1}, abflag::breath },
    { ABIL_BREATHE_MAGMA, "Breathe Magma",
      0, 0, 250, 0, {fail_basis::xl, 30, 1}, abflag::breath },
    { ABIL_BREATHE_FROST, "Breathe Frost",
      0, 0, 250, 0, {fail_basis::xl, 30, 1}, abflag::breath },
    { ABIL_BREATHE_POISON, "Breathe Poison Gas",
      0, 0, 250, 0, {fail_basis::xl, 30, 1}, abflag::breath },
    { ABIL_BREATHE_MEPHITIC, "Breathe Noxious Fumes",
      0, 0, 250, 0, {fail_basis::xl, 30, 1}, abflag::breath },
    { ABIL_BREATHE_LIGHTNING, "Breathe Lightning",
      0, 0, 250, 0, {fail_basis::xl, 30, 1}, abflag::breath },
    { ABIL_BREATHE_POWER, "Breathe Dispelling Energy",
      0, 0, 250, 0, {fail_basis::xl, 30, 1}, abflag::breath },
    { ABIL_BREATHE_FOG, "Breathe Fog",
      0, 0, 250, 0, {fail_basis::xl, 20, 1}, abflag::breath },
    { ABIL_BREATHE_STEAM, "Breathe Steam",
      0, 0, 250, 0, {fail_basis::xl, 20, 1}, abflag::breath },
    { ABIL_BREATHE_DRAIN, "Breathe Negative Energy",
      0, 0, 250, 0,{ fail_basis::xl, 30, 1 }, abflag::breath },
    { ABIL_BREATHE_MIASMA, "Breathe Foul Miasma",
      0, 0, 250, 0,{ fail_basis::xl, 30, 1 }, abflag::breath },
    { ABIL_BREATHE_SILVER, "Breathe Silver Splinters",
      0, 0, 250, 0,{ fail_basis::xl, 30, 1 }, abflag::breath },
    { ABIL_BREATHE_WIND, "Breathe Blast of Wind",
      0, 0, 250, 0,{ fail_basis::xl, 30, 1 }, abflag::breath },
    { ABIL_BREATHE_BLOOD, "Breathe Vampiric Fog",
      0, 0, 250, 0,{ fail_basis::xl, 30, 1 }, abflag::breath },
    { ABIL_BREATHE_HOLY_FLAMES, "Breathe Blessed Flames",
      0, 0, 250, 0,{ fail_basis::xl, 30, 1 }, abflag::breath },
    { ABIL_BREATHE_BUTTERFLIES, "Breathe Butterflies",
      0, 0, 250, 0,{ fail_basis::xl, 30, 1 }, abflag::breath },
    { ABIL_BREATHE_BONE, "Breathe Bone Shards",
      0, 0, 250, 0,{ fail_basis::xl, 30, 1 }, abflag::breath },
    { ABIL_BREATHE_CHAOS, "Breathe Seething Chaos",
      0, 0, 250, 0,{ fail_basis::xl, 30, 1 }, abflag::breath },
    { ABIL_BREATHE_GHOSTLY_FLAMES, "Breathe Spectral Mist",
      0, 0, 250, 0,{ fail_basis::xl, 30, 1 }, abflag::breath },
    { ABIL_BREATHE_METAL, "Breathe Metal Splinters",
      0, 0, 250, 0,{ fail_basis::xl, 30, 1 }, abflag::breath },
    { ABIL_BREATHE_RADIATION, "Breathe Mutagenic Radiation",
      0, 0, 250, 0,{ fail_basis::xl, 30, 1 }, abflag::breath },
    { ABIL_BREATHE_ACID, "Breathe Acid",
      0, 0, 250, 0, {fail_basis::xl, 30, 1}, abflag::breath },
    { ABIL_BREATHE_DART, "Breathe Dart",
      0, 0, 250, 0, {fail_basis::xl, 30, 1}, abflag::breath },
    { ABIL_BREATHE_TRIPLE, "Breathe Searing Ice",
      0, 0, 250, 0, {fail_basis::xl, 30, 1}, abflag::breath }, // Placeholder; only used by a god power.

    { ABIL_TRAN_BAT, "Bat Form",
      2, 0, 0, 0, {fail_basis::xl, 45, 2}, abflag::starve_ok },

    { ABIL_FLY, "Fly", 3, 0, 200, 0, {fail_basis::xl, 42, 3}, abflag::none },
    { ABIL_STOP_FLYING, "Stop Flying", 0, 0, 0, 0, {}, abflag::starve_ok },
    { ABIL_PLANT_ROOTS, "Plant Roots", 3, 0, 180, 0,{ fail_basis::xl, 42, 3 }, abflag::none },
    { ABIL_DEROOT, "Unearth Roots", 0, 0, 0, 0,{}, abflag::starve_ok },
    { ABIL_HELLFIRE, "Hurl Hellfire",
      0, 150, 400, 0, {fail_basis::xl, 50, 1}, abflag::none },

    { ABIL_TURN_INVISIBLE, "Turn Invisible",
      0, 100, 200, 0, {fail_basis::xl, 50, 2}, abflag::none },
    { ABIL_BUD_EYEBALLS, "Spawn Eyeballs",
      0, 200, 400, 0, {fail_basis::xl, 50, 2}, abflag::none },
    { ABIL_SILENT_SCREAM, "Silent Scream",
      0, 100, 200, 0, {fail_basis::xl, 50, 2}, abflag::none },
    { ABIL_FROST_BURST, "Frost Burst",
      0, 200, 400, 0, {fail_basis::xl, 50, 2}, abflag::none },
    { ABIL_CORROSIVE_WAVE, "Corrosive Wave",
      0, 100, 200, 0, {fail_basis::xl, 50, 2}, abflag::none },
    { ABIL_SLIME_BOLT, "Fluid Rush",
      0, 200, 400, 0, {fail_basis::xl, 50, 2}, abflag::none },
    { ABIL_SUBSUME, "Subsume Item", 0, 0, 0, 0,{}, abflag::starve_ok },
    { ABIL_EJECT, "Eject Item", 0, 0, 0, 0,{}, abflag::starve_ok },

    { ABIL_CANCEL_PPROJ, "Cancel Portal Projectile",
      0, 0, 0, 0, {}, abflag::instant | abflag::starve_ok },

    { ABIL_DIG, "Dig", 0, 0, 0, 0, {}, abflag::instant | abflag::starve_ok },
    { ABIL_SHAFT_SELF, "Shaft Self", 0, 0, 500, 0, {}, abflag::delay },

    { ABIL_HOP, "Hop", 0, 0, 0, 0, {}, abflag::none },

    { ABIL_SPIDER_JUMP, "Spider Jump", 0, 0, 0, 0, {fail_basis::spider, 60, 8}, abflag::none },
    { ABIL_SPIDER_WEB, "Web Snare", 0, 0, 0, 0, {fail_basis::spider, 80, 6}, abflag::none },

    // EVOKE abilities use Evocations and come from items.
    // Teleportation and Blink can also come from mutations
    // so we have to distinguish them (see above). The off items
    // below are labeled EVOKE because they only work now if the
    // player has an item with the evocable power (not just because
    // you used a wand, potion, or miscast effect). I didn't see
    // any reason to label them as "Evoke" in the text, they don't
    // use or train Evocations (the others do).  -- bwr
    { ABIL_EVOKE_BLINK, "Evoke Blink",
      1, 0, 120, 0, {fail_basis::evo, 40, 2}, abflag::none },
    { ABIL_HEAL_WOUNDS, "Heal Wounds",
      0, 0, 0, 0, {fail_basis::xl, 45, 2}, abflag::none },
    { ABIL_EVOKE_BERSERK, "Evoke Berserk Rage",
      0, 0, 1000, 0, {fail_basis::evo, 50, 2}, abflag::none },

    { ABIL_EVOKE_TURN_INVISIBLE, "Evoke Invisibility",
      2, 0, 500, 0, {fail_basis::evo, 60, 2}, abflag::none },
#if TAG_MAJOR_VERSION == 34
    { ABIL_EVOKE_TURN_VISIBLE, "Turn Visible",
      0, 0, 0, 0, {}, abflag::starve_ok },
#endif
    { ABIL_EVOKE_FLIGHT, "Evoke Flight",
      1, 0, 150, 0, {fail_basis::evo, 40, 2}, abflag::none },
    { ABIL_EVOKE_FOG, "Evoke Fog",
      2, 0, 500, 0, {fail_basis::evo, 50, 2}, abflag::none },
    { ABIL_EVOKE_RATSKIN, "Evoke Ratskin",
      3, 0, 300, 0, {fail_basis::evo, 50, 2}, abflag::none },
    { ABIL_EVOKE_THUNDER, "Evoke Thunderclouds",
      5, 0, 300, 0, {fail_basis::evo, 60, 2}, abflag::none },


    { ABIL_END_TRANSFORMATION, "End Transformation",
      0, 0, 0, 0, {}, abflag::starve_ok },
    { ABIL_END_UPRISING, "End Uprising",
        0, 0, 0, 0,{}, abflag::starve_ok },
    { ABIL_DISMOUNT, "Dismount",
        0, 0, 0, 0,{}, abflag::starve_ok },


    // INVOCATIONS:
    // Zin
    { ABIL_ZIN_RECITE, "Recite",
      0, 0, 0, 0, {fail_basis::invo, 30, 6, 20}, abflag::none },
    { ABIL_ZIN_VITALISATION, "Vitalisation",
      2, 0, 0, 2, {fail_basis::invo, 40, 5, 20}, abflag::none },
    { ABIL_ZIN_IMPRISON, "Imprison",
      5, 0, 0, 6, {fail_basis::invo, 60, 5, 20}, abflag::none },
    { ABIL_ZIN_SANCTUARY, "Sanctuary",
      7, 0, 0, 20, {fail_basis::invo, 80, 4, 25}, abflag::none },
    { ABIL_ZIN_DONATE_GOLD, "Donate Gold",
      0, 0, 0, 0, {fail_basis::invo}, abflag::none },

    // The Shining One
    { ABIL_TSO_DIVINE_SHIELD, "Divine Shield",
      3, 0, 100, 3, {fail_basis::invo, 40, 5, 20}, abflag::none },
    { ABIL_TSO_CLEANSING_FLAME, "Cleansing Flame",
      5, 0, 250, 3, {fail_basis::invo, 70, 4, 25}, abflag::none },
    { ABIL_TSO_SUMMON_DIVINE_WARRIOR, "Summon Divine Warrior",
      8, 0, 400, 8, {fail_basis::invo, 80, 4, 25}, abflag::none },
    { ABIL_TSO_BLESS_WEAPON, "Brand Weapon With Holy Wrath", 0, 0, 0, 0,
      {fail_basis::invo}, abflag::none },

    // Kikubaaqudgha
    { ABIL_KIKU_OPEN_CRYPTS, "Open Crypts",
        0, 0, 0, 0, {fail_basis::invo}, abflag::instant | abflag::starve_ok },
    { ABIL_KIKU_CLOSE_CRYPTS, "Close Crypts",
        0, 0, 0, 0, {fail_basis::invo}, abflag::instant | abflag::starve_ok },
    { ABIL_KIKU_GIFT_NECRONOMICON, "Receive Necronomicon", 0, 0, 0, 0,
      {fail_basis::invo}, abflag::none },
    { ABIL_KIKU_BLESS_WEAPON, "Brand Weapon With Pain", 0, 0, 0, 0,
      {fail_basis::invo}, abflag::pain },

    // Yredelemnul
    { ABIL_YRED_INJURY_MIRROR, "Injury Mirror",
      4, 0, 0, 20, {fail_basis::invo, 40, 4, 20}, abflag::none },
    { ABIL_YRED_ANIMATE_REMAINS, "Twisted Reanimation",
      4, 0, 500, 0, {fail_basis::invo, 40, 4, 20}, abflag::none },
    { ABIL_YRED_RECALL_UNDEAD_SLAVES, "Recall Undead Slaves",
      3, 0, 0, 0, {fail_basis::invo, 50, 4, 20}, abflag::none },
    { ABIL_YRED_ENSLAVE_SOUL, "Enslave Soul",
      8, 0, 1200, 10, {fail_basis::invo, 80, 4, 25}, abflag::none },
    { ABIL_YRED_DRAIN_LIFE, "Drain Life",
        9, 0, 1600, 20, {fail_basis::invo, 100, 4, 25}, abflag::none },

    // Okawaru
    { ABIL_OKAWARU_HEROISM, "Heroism",
      2, 0, 0, 2, {fail_basis::invo, 30, 6, 20}, abflag::none },
    { ABIL_OKAWARU_FINESSE, "Finesse",
      5, 0, 0, 6, {fail_basis::invo, 60, 4, 25}, abflag::none },

    // Makhleb
    { ABIL_MAKHLEB_MINOR_DESTRUCTION, "Minor Destruction",
      0, scaling_cost::fixed(1), 0, 0, {fail_basis::invo, 40, 5, 20}, abflag::none },
    { ABIL_MAKHLEB_LESSER_SERVANT_OF_MAKHLEB, "Lesser Servant of Makhleb",
      0, scaling_cost::fixed(4), 0, 3, {fail_basis::invo, 40, 5, 20}, abflag::hostile },
    { ABIL_MAKHLEB_MAJOR_DESTRUCTION, "Major Destruction",
      0, scaling_cost::fixed(6), 0, 1,
      {fail_basis::invo, 60, 4, 25}, abflag::none },
    { ABIL_MAKHLEB_GREATER_SERVANT_OF_MAKHLEB, "Greater Servant of Makhleb",
      0, scaling_cost::fixed(10), 0, 8,
      {fail_basis::invo, 90, 2, 5}, abflag::hostile },

    // Sif Muna
    { ABIL_SIF_MUNA_DIVINE_ENERGY, "Divine Energy",
      0, 0, 0, 0, {fail_basis::invo}, abflag::instant | abflag::starve_ok },
    { ABIL_SIF_MUNA_STOP_DIVINE_ENERGY, "Stop Divine Energy",
      0, 0, 0, 0, {fail_basis::invo}, abflag::instant | abflag::starve_ok },
    { ABIL_SIF_MUNA_FORGET_SPELL, "Forget Spell",
      0, 0, 0, 12, {fail_basis::invo}, abflag::none },
    { ABIL_SIF_MUNA_CHANNEL_ENERGY, "Channel Magic",
      0, 0, 500, 3, {fail_basis::invo, 60, 4, 25}, abflag::none },

    // Trog
    { ABIL_TROG_BERSERK, "Berserk",
      0, 0, 1200, 0, {fail_basis::invo}, abflag::none },
    { ABIL_TROG_REGEN_MR, "Trog's Hand",
      0, 0, 400, 3, {fail_basis::invo, piety_breakpoint(2), 0, 1}, abflag::berserk_only },
    { ABIL_TROG_BROTHERS_IN_ARMS, "Brothers in Arms",
      0, 0, 750, 6,
      {fail_basis::invo, piety_breakpoint(5), 0, 1}, abflag::berserk_only },

    // Elyvilon
    { ABIL_ELYVILON_LIFESAVING, "Divine Protection",
      0, 0, 0, 0, {fail_basis::invo}, abflag::piety },
    { ABIL_ELYVILON_LESSER_HEALING, "Lesser Healing",
      1, 0, 200, 1, {fail_basis::invo, 30, 6, 20}, abflag::none },
    { ABIL_ELYVILON_HEAL_OTHER, "Heal Other",
      2, 0, 350, 3, {fail_basis::invo, 40, 5, 20}, abflag::none },
    { ABIL_ELYVILON_PURIFICATION, "Purification",
      3, 0, 450, 5, {fail_basis::invo, 20, 5, 20}, abflag::conf_ok },
    { ABIL_ELYVILON_GREATER_HEALING, "Greater Healing",
      2, 0, 750, 5, {fail_basis::invo, 40, 5, 20}, abflag::none },
    { ABIL_ELYVILON_DIVINE_VIGOUR, "Divine Vigour",
      0, 0, 1200, 9, {fail_basis::invo, 80, 4, 25}, abflag::none },

    // Lugonu
    { ABIL_LUGONU_ABYSS_EXIT, "Depart the Abyss",
      1, 0, 0, 15, {fail_basis::invo, 30, 6, 20}, abflag::none },
    { ABIL_LUGONU_BEND_SPACE, "Bend Space",
      1, scaling_cost::fixed(2), 0, 0, {fail_basis::invo, 40, 5, 20}, abflag::none },
    { ABIL_LUGONU_BANISH, "Banish", 4, 0, 600, 4,
      {fail_basis::invo, 85, 7, 20}, abflag::none },
    { ABIL_LUGONU_CORRUPT, "Corrupt", 7, scaling_cost::fixed(5), 1500, 15,
      {fail_basis::invo, 70, 4, 25}, abflag::none },
    { ABIL_LUGONU_ABYSS_ENTER, "Enter the Abyss", 10, 0, 1500, 42,
      {fail_basis::invo, 80, 4, 25}, abflag::pain },
    { ABIL_LUGONU_BLESS_WEAPON, "Brand Weapon With Distortion", 0, 0, 0, 0,
      {fail_basis::invo}, abflag::none },

    // Nemelex
    { ABIL_NEMELEX_TRIPLE_DRAW, "Triple Draw",
      2, 0, 0, 3, {fail_basis::invo, 60, 5, 20}, abflag::none },
    { ABIL_NEMELEX_DEAL_FOUR, "Deal Four",
      8, 0, 0, 12, {fail_basis::invo, -1}, abflag::none }, // failure special-cased
    { ABIL_NEMELEX_STACK_FIVE, "Stack Five",
      5, 0, 0, 15, {fail_basis::invo, 80, 4, 25}, abflag::none },

    // Beogh
    { ABIL_BEOGH_SMITING, "Smiting",
      3, 0, 0, 2, {fail_basis::invo, 40, 5, 20}, abflag::none },
    { ABIL_BEOGH_RECALL_ORCISH_FOLLOWERS, "Recall Orcish Followers",
      2, 0, 0, 0, {fail_basis::invo, 30, 6, 20}, abflag::none },
    { ABIL_BEOGH_GIFT_ITEM, "Give Item to Named Follower",
      0, 0, 0, 0, {fail_basis::invo}, abflag::none },
    { ABIL_BEOGH_RESURRECTION, "Resurrection",
      0, 0, 0, 25, {fail_basis::invo}, abflag::none },

    // Jiyva
    { ABIL_JIYVA_DISSOLUTION, "Dissolution",
      4, 0, 250, 2, {fail_basis::invo, 40, 4, 25}, abflag::none },
    { ABIL_JIYVA_SET_TARGETS_FREE, "Set Stat Targets",
      0, 0, 0, 0, {fail_basis::invo}, abflag::instant },
    { ABIL_JIYVA_SET_TARGETS, "Set Stat Targets",
      0, 0, 0, 4, {fail_basis::invo}, abflag::instant },
    { ABIL_JIYVA_SLIME_MOUNT, "Oozing Slime Mount",
      8, 0, 1000, 8, { fail_basis::invo, 90, 6, 10 }, abflag::none },
    { ABIL_JIYVA_SLIMIFY, "Slimify",
      6, 0, 500, 12, { fail_basis::invo, 108, 6, 10 }, abflag::none },

    // Fedhas
    { ABIL_FEDHAS_FUNGAL_BLOOM, "Fungal Bloom",
      0, 0, 0, 0, {fail_basis::invo}, abflag::none },
    { ABIL_FEDHAS_SUNLIGHT, "Sunlight",
      2, 0, 75, 0, {fail_basis::invo, 30, 6, 20}, abflag::none },
    { ABIL_FEDHAS_EVOLUTION, "Evolution",
      2, 0, 250, 0, {fail_basis::invo, 30, 6, 20}, abflag::none },
    { ABIL_FEDHAS_PLANT_RING, "Growth",
      8, 0, 300, 15, {fail_basis::invo, 40, 5, 20}, abflag::none },
    { ABIL_FEDHAS_SPAWN_SPORES, "Reproduction",
      4, 0, 300, 2, {fail_basis::invo, 60, 4, 25}, abflag::none },
    { ABIL_FEDHAS_RAIN, "Rain",
      4, 0, 450, 6, {fail_basis::invo, 70, 4, 25}, abflag::none },

    // Cheibriados
    { ABIL_CHEIBRIADOS_TIME_BEND, "Bend Time",
      3, 0, 100, 2, {fail_basis::invo, 40, 4, 20}, abflag::none },
    { ABIL_CHEIBRIADOS_DISTORTION, "Temporal Distortion",
      4, 0, 300, 5, {fail_basis::invo, 60, 5, 20}, abflag::instant },
    { ABIL_CHEIBRIADOS_SLOUCH, "Slouch",
      5, 0, 200, 12, {fail_basis::invo, 60, 4, 25}, abflag::none },
    { ABIL_CHEIBRIADOS_TIME_STEP, "Step From Time",
      10, 0, 500, 15, {fail_basis::invo, 80, 4, 25}, abflag::none },

    // Ashenzari
    { ABIL_ASHENZARI_CURSE, "Curse Item",
      0, 0, 0, 0, {fail_basis::invo}, abflag::none },
    { ABIL_ASHENZARI_SCRYING, "Scrying",
      0, 0, 0, 0,{ fail_basis::invo }, abflag::instant },
    { ABIL_ASHENZARI_TRANSFER_KNOWLEDGE, "Transfer Knowledge",
      0, 0, 0, 15, {fail_basis::invo}, abflag::none },
    { ABIL_ASHENZARI_END_TRANSFER, "End Transfer Knowledge",
      0, 0, 0, 0, {fail_basis::invo}, abflag::starve_ok },

    // Dithmenos
    { ABIL_DITHMENOS_SHADOW_STEP, "Shadow Step",
      4, 80, 0, 8, {fail_basis::invo, 30, 6, 20}, abflag::none },
    { ABIL_DITHMENOS_SHADOW_FORM, "Shadow Form",
      9, 0, 0, 18, {fail_basis::invo, 80, 4, 25}, abflag::skill_drain },

    // Ru
    { ABIL_RU_DRAW_OUT_POWER, "Draw Out Power", 0, 0, 100, 0,
      {fail_basis::invo}, abflag::exhaustion | abflag::skill_drain | abflag::conf_ok | abflag::silence_ok },
    { ABIL_RU_POWER_LEAP, "Power Leap",
      5, 0, 250, 0, {fail_basis::invo}, abflag::exhaustion | abflag::silence_ok },
    { ABIL_RU_APOCALYPSE, "Apocalypse",
      8, 0, 800, 0, {fail_basis::invo}, abflag::exhaustion | abflag::skill_drain | abflag::silence_ok },

    { ABIL_RU_SACRIFICE_PURITY, "Sacrifice Purity",
      0, 0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
    { ABIL_RU_SACRIFICE_WORDS, "Sacrifice Words",
      0, 0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
    { ABIL_RU_SACRIFICE_DRINK, "Sacrifice Drink",
      0, 0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
    { ABIL_RU_SACRIFICE_ESSENCE, "Sacrifice Essence",
      0, 0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
    { ABIL_RU_SACRIFICE_HEALTH, "Sacrifice Health",
      0, 0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
    { ABIL_RU_SACRIFICE_STEALTH, "Sacrifice Stealth",
      0, 0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
    { ABIL_RU_SACRIFICE_ARTIFICE, "Sacrifice Artifice",
      0, 0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
    { ABIL_RU_SACRIFICE_LOVE, "Sacrifice Love",
      0, 0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
    { ABIL_RU_SACRIFICE_COURAGE, "Sacrifice Courage",
      0, 0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
    { ABIL_RU_SACRIFICE_ARCANA, "Sacrifice Arcana",
      0, 0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
    { ABIL_RU_SACRIFICE_NIMBLENESS, "Sacrifice Nimbleness",
      0, 0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
    { ABIL_RU_SACRIFICE_DURABILITY, "Sacrifice Durability",
      0, 0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
    { ABIL_RU_SACRIFICE_HAND, "Sacrifice a Hand",
      0, 0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
    { ABIL_RU_SACRIFICE_EXPERIENCE, "Sacrifice Experience",
      0, 0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
    { ABIL_RU_SACRIFICE_SKILL, "Sacrifice Skill",
      0, 0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
    { ABIL_RU_SACRIFICE_EYE, "Sacrifice an Eye",
      0, 0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
    { ABIL_RU_SACRIFICE_RESISTANCE, "Sacrifice Resistance",
      0, 0, 0, 0, {fail_basis::invo}, abflag::sacrifice },
    { ABIL_RU_REJECT_SACRIFICES, "Reject Sacrifices",
      0, 0, 0, 0, {fail_basis::invo}, abflag::none },

    // Gozag
    { ABIL_GOZAG_POTION_PETITION, "Potion Petition",
      0, 0, 0, 0, {fail_basis::invo}, abflag::gold },
    { ABIL_GOZAG_CALL_MERCHANT, "Call Merchant",
      0, 0, 0, 0, {fail_basis::invo}, abflag::gold|abflag::starve_ok },
    { ABIL_GOZAG_BRIBE_BRANCH, "Bribe Branch",
      0, 0, 0, 0, {fail_basis::invo}, abflag::gold },

    // Qazlal
    { ABIL_QAZLAL_UPHEAVAL, "Upheaval",
      4, 0, 250, 5, {fail_basis::invo, 40, 5, 20}, abflag::none },
    { ABIL_QAZLAL_ELEMENTAL_FORCE, "Elemental Force",
      6, 0, 500, 9, {fail_basis::invo, 60, 5, 20}, abflag::none },
    { ABIL_QAZLAL_DISASTER_AREA, "Disaster Area",
      7, 0, 1000, 15, {fail_basis::invo, 70, 4, 25}, abflag::none },

#if TAG_MAJOR_VERSION == 34
    // Pakellas
    { ABIL_PAKELLAS_DEVICE_SURGE, "Device Surge",
      0, 0, 0, 1,
      {fail_basis::invo, 40, 5, 20}, abflag::instant },
#endif

    // Uskayaw
    { ABIL_USKAYAW_STOMP, "Stomp",
        3, 0, 200, 20, {fail_basis::invo}, abflag::none },
    { ABIL_USKAYAW_LINE_PASS, "Line Pass",
        4, 0, 400, 20, {fail_basis::invo}, abflag::none},
    { ABIL_USKAYAW_GRAND_FINALE, "Grand Finale",
        8, 0, 1000, 0,
        {fail_basis::invo, 120 + piety_breakpoint(4), 5, 1}, abflag::none},

    // Hepliaklqana
    { ABIL_HEPLIAKLQANA_RECALL, "Recall Ancestor",
        2, 0, 0, 0, {fail_basis::invo}, abflag::none },
    { ABIL_HEPLIAKLQANA_TRANSFERENCE, "Transference",
        2, 0, 0, 5, {fail_basis::invo, 40, 5, 20},
        abflag::none },
    { ABIL_HEPLIAKLQANA_IDEALISE, "Idealise",
        4, 0, 0, 6, {fail_basis::invo, 60, 4, 25},
        abflag::none },

    { ABIL_HEPLIAKLQANA_TYPE_KNIGHT,       "Ancestor Life: Knight",
        0, 0, 0, 0, {fail_basis::invo}, abflag::starve_ok },
    { ABIL_HEPLIAKLQANA_TYPE_BATTLEMAGE,   "Ancestor Life: Battlemage",
        0, 0, 0, 0, {fail_basis::invo}, abflag::starve_ok },
    { ABIL_HEPLIAKLQANA_TYPE_HEXER,        "Ancestor Life: Hexer",
        0, 0, 0, 0, {fail_basis::invo}, abflag::starve_ok },

    { ABIL_HEPLIAKLQANA_IDENTITY,  "Ancestor Identity",
        0, 0, 0, 0, {fail_basis::invo}, abflag::instant | abflag::starve_ok },

    // Wu Jian
    { ABIL_WU_JIAN_SERPENTS_LASH, "Serpent's Lash",
        0, 0, 100, 3, {fail_basis::invo}, abflag::exhaustion | abflag::instant },
    { ABIL_WU_JIAN_HEAVENLY_STORM, "Heavenly Storm",
        0, 0, 500, 30, {fail_basis::invo, piety_breakpoint(5), 0, 1}, abflag::none },
    // Lunge and Whirlwind abilities aren't menu abilities but currently need
    // to exist for action counting, hence need enums/entries.
    { ABIL_WU_JIAN_LUNGE, "Lunge", 0, 0, 0, 0, {}, abflag::berserk_ok },
    { ABIL_WU_JIAN_WHIRLWIND, "Whirlwind", 0, 0, 0, 0, {}, abflag::berserk_ok },
    { ABIL_WU_JIAN_WALLJUMP, "Wall Jump",
        0, 0, 0, 0, {}, abflag::starve_ok | abflag::berserk_ok },

    // Bahamut and Tiamat
        // Choices
    { ABIL_BAHAMUT_PROTECTION, "Choose Bahamut's Protection",
        0, 0, 0, 0, { fail_basis::invo }, abflag::none },
    { ABIL_TIAMAT_RETRIBUTION, "Choose Tiamat's Retribution",
        0, 0, 0, 0, { fail_basis::invo }, abflag::none },
    { ABIL_CHOOSE_BAHAMUT_BREATH, "Choose Bahamut's Refined Breath",
        0, 0, 0, 0, { fail_basis::invo }, abflag::none },
    { ABIL_CHOOSE_TIAMAT_BREATH, "Choose Tiamat's Adaptive Breath",
        0, 0, 0, 0, { fail_basis::invo }, abflag::none },
    { ABIL_CHOOSE_BAHAMUT_DRAKE, "Choose Bahamut's Drake Mount",
        0, 0, 0, 0, { fail_basis::invo }, abflag::none },
    { ABIL_CHOOSE_TIAMAT_DRAKE, "Choose Tiamat's Summon Drakes",
        0, 0, 0, 0, { fail_basis::invo }, abflag::none },
    { ABIL_BAHAMUT_TRANSFORM, "Choose Bahamut's Promotion",
        0, 0, 0, 0, { fail_basis::invo }, abflag::none },
    { ABIL_CHOOSE_TIAMAT_TRANSFORM, "Choose Tiamat's Transformation",
        0, 0, 0, 0, { fail_basis::invo }, abflag::none },
    { ABIL_BAHAMUT_DRAGONSLAYING, "Brand Weapon with Dragonslaying",
        0, 0, 0, 0, { fail_basis::invo }, abflag::none },
    { ABIL_TIAMAT_DRAGON_BOOK, "Receive Book of the Dragon",
        0, 0, 0, 0, { fail_basis::invo }, abflag::none },
        // Normal Actives
    { ABIL_BAHAMUT_EMPOWERED_BREATH, "Enhanced Breath",
        4, 0, 400, 4, { fail_basis::invo, 60, 5, 20 }, abflag::breath },
    { ABIL_TIAMAT_ADAPTIVE_BREATH, "Adaptive Breath",
        4, 0, 300, 4, { fail_basis::invo, 60, 5, 20 }, abflag::breath },
    { ABIL_BAHAMUT_DRAKE_MOUNT, "Drake Mount",
        7, 0, 750, 12, { fail_basis::invo, 90, 6, 10 }, abflag::none },
    { ABIL_TIAMAT_SUMMON_DRAKES, "Summon Drakes",
        7, 0, 750, 12, { fail_basis::invo, 90, 6, 10 }, abflag::none },
    { ABIL_TIAMAT_TRANSFORM, "Change Draconian Colour",
        4, 0, 400, 8, { fail_basis::invo, 120, 5, 10 }, abflag::none },

    { ABIL_STOP_RECALL, "Stop Recall", 0, 0, 0, 0, {fail_basis::invo}, abflag::starve_ok },
    { ABIL_RENOUNCE_RELIGION, "Renounce Religion",
      0, 0, 0, 0, {fail_basis::invo}, abflag::starve_ok | abflag::silence_ok },
    { ABIL_CONVERT_TO_BEOGH, "Convert to Beogh",
      0, 0, 0, 0, {fail_basis::invo}, abflag::starve_ok },
};

static const ability_def& get_ability_def(ability_type abil)
{
    for (const ability_def &ab_def : Ability_List)
        if (ab_def.ability == abil)
            return ab_def;

    return Ability_List[0];
}

unsigned int ability_mp_cost(ability_type abil)
{
    if (you.species == SP_FAIRY && get_ability_def(abil).mp_cost > 1)
        return 1;
    return get_ability_def(abil).mp_cost;
}

/**
 * Is there a valid ability with a name matching that given?
 *
 * @param key   The name in question. (Not case sensitive.)
 * @return      true if such an ability exists; false if not.
 */
bool string_matches_ability_name(const string& key)
{
    return ability_by_name(key) != ABIL_NON_ABILITY;
}

/**
 * Find an ability whose name matches the given key.
 *
 * @param name      The name in question. (Not case sensitive.)
 * @return          The enum of the relevant ability, if there was one; else
 *                  ABIL_NON_ABILITY.
 */
ability_type ability_by_name(const string &key)
{
    for (const auto &abil : Ability_List)
    {
        if (abil.ability == ABIL_NON_ABILITY)
            continue;

        const string name = lowercase_string(ability_name(abil.ability));
        if (name == lowercase_string(key))
            return abil.ability;
    }

    return ABIL_NON_ABILITY;
}

string print_abilities()
{
    string text = "\n<w>a:</w> ";

    const vector<talent> talents = your_talents(false);

    if (talents.empty())
        text += "no special abilities";
    else
    {
        for (unsigned int i = 0; i < talents.size(); ++i)
        {
            if (i)
                text += ", ";
            text += ability_name(talents[i].which);
        }
    }

    return text;
}

int get_gold_cost(ability_type ability)
{
    switch (ability)
    {
    case ABIL_GOZAG_CALL_MERCHANT:
        return gozag_price_for_shop(true);
    case ABIL_GOZAG_POTION_PETITION:
        return gozag_potion_price();
    case ABIL_GOZAG_BRIBE_BRANCH:
        return GOZAG_BRIBE_AMOUNT;
    default:
        return 0;
    }
}

const string make_cost_description(ability_type ability)
{
    const ability_def& abil = get_ability_def(ability);
    string ret;
    if (ability_mp_cost(abil.ability))
        ret += make_stringf(", %d MP", ability_mp_cost(abil.ability));

    if (abil.hp_cost)
        ret += make_stringf(", %d HP", abil.hp_cost.cost(you.hp_max));

    if (abil.food_cost && !you_foodless())
        ret += make_stringf(", %d Nutr", abil.food_cost);

    if (abil.piety_cost)
        ret += make_stringf(", %d Piety", abil.piety_cost);
    
    if (abil.flags & abflag::piety)
        ret += (", Variable Piety"); // randomised and exact amount hidden from player

    if (abil.flags & abflag::breath)
        ret += ", Breath";

    if (abil.flags & abflag::delay)
        ret += ", Delay";

    if (abil.flags & abflag::pain)
        ret += ", Pain";

    if (abil.flags & abflag::exhaustion)
        ret += ", Exhaustion";

    if (bool(abil.flags & abflag::instant) && (abil.ability != ABIL_CHEIBRIADOS_DISTORTION))
        ret += ", Instant"; // not really a cost, more of a bonus - bwr

    if (abil.flags & abflag::skill_drain)
        ret += ", Skill drain";

    if (abil.flags & abflag::gold)
    {
        const int amount = get_gold_cost(ability);
        if (amount)
            ret += make_stringf(", %d Gold", amount);
        else if (ability == ABIL_GOZAG_POTION_PETITION)
            ret += ", Free";
        else
            ret += ", Gold";
    }

    if (abil.flags & abflag::sacrifice)
    {
        ret += ", ";
        const string prefix = "Sacrifice ";
        ret += string(ability_name(ability)).substr(prefix.size());
        ret += ru_sac_text(ability);
    }

    // If we haven't output anything so far, then the effect has no cost
    if (ret.empty())
        return "None";

    ret.erase(0, 2);
    return ret;
}

// Ripped this out both because it's used a lot and because it will likely become more complicated 
// later so keeping it centralized for all calls will make the later mutations easier.
int drac_breath_power(bool empowered)
{
    int power =  (you.form == transformation::dragon) ? 2 * you.experience_level 
                                                      : you.experience_level;
    if (empowered)
        power += you.skill(SK_INVOCATIONS);
    return power;
}

static const string _detailed_cost_description(ability_type ability)
{
    const ability_def& abil = get_ability_def(ability);
    ostringstream ret;

    bool have_cost = false;
    ret << "This ability costs: ";

    if (ability_mp_cost(abil.ability) > 0)
    {
        have_cost = true;
        ret << "\nMP     : ";
        ret << ability_mp_cost(abil.ability);
    }
    if (abil.hp_cost)
    {
        have_cost = true;
        ret << "\nHP     : ";
        ret << abil.hp_cost.cost(you.hp_max);
    }

    if (abil.food_cost && !you_foodless())
    {
        have_cost = true;
        ret << "\nHunger : ";
        ret << hunger_cost_string(abil.food_cost);
    }

    if (abil.piety_cost || abil.flags & abflag::piety)
    {
        have_cost = true;
        ret << "\nPiety  : ";
        if (abil.flags & abflag::piety)
            ret << "variable";
        else
            ret << abil.piety_cost;
    }

    if (abil.flags & abflag::gold)
    {
        have_cost = true;
        ret << "\nGold   : ";
        int gold_amount = get_gold_cost(ability);
        if (gold_amount)
            ret << gold_amount;
        else if (ability == ABIL_GOZAG_POTION_PETITION)
            ret << "free";
        else
            ret << "variable";
    }

    if (!have_cost)
        ret << "nothing.";

    if (abil.flags & abflag::breath)
        ret << "\nYou must catch your breath between uses of this ability.";

    if (abil.flags & abflag::delay)
        ret << "\nIt takes some time before being effective.";

    if (abil.flags & abflag::pain)
        ret << "\nUsing this ability will hurt you.";

    if (abil.flags & abflag::exhaustion)
        ret << "\nIt causes exhaustion, and cannot be used when exhausted.";

    if (abil.flags & abflag::instant)
        ret << "\nIt is instantaneous.";

    if (abil.flags & abflag::conf_ok)
        ret << "\nYou can use it even if confused.";

    if (abil.flags & abflag::skill_drain)
        ret << "\nIt will temporarily drain your skills when used.";

    if (abil.ability == ABIL_HEAL_WOUNDS)
    {
        ret << "\nIt has a chance of reducing your maximum magic capacity "
               "when used.";
    }

    return ret.str();
}

static int _slime_count(bool clear = false)
{
    int count = 0;
    for (map_marker *mark : env.markers.get_all(MAT_TERRAIN_CHANGE))
    {
        map_terrain_change_marker *marker =
            dynamic_cast<map_terrain_change_marker*>(mark);

        if (marker->change_type == TERRAIN_CHANGE_SLIME
            && you.see_cell_no_trans(marker->pos))
        {
            count++;
            if (clear)
                marker->duration = 0;
        }
    }
    return count;
}

ability_type fixup_ability(ability_type ability)
{
    switch (ability)
    {
    case ABIL_YRED_ANIMATE_REMAINS:
        return ability;

    case ABIL_YRED_RECALL_UNDEAD_SLAVES:
    case ABIL_BEOGH_RECALL_ORCISH_FOLLOWERS:
        if (!you.recall_list.empty())
            return ABIL_STOP_RECALL;
        return ability;

    case ABIL_EVOKE_BERSERK:
    case ABIL_TROG_BERSERK:
        if (you.is_lifeless_undead(false)
            || you.get_mutation_level(MUT_STASIS))
        {
            return ABIL_NON_ABILITY;
        }
        return ability;

    case ABIL_BLINK:
    case ABIL_EVOKE_BLINK:
        if (you.get_mutation_level(MUT_STASIS))
            return ABIL_NON_ABILITY;
        return ability;

    case ABIL_JIYVA_SET_TARGETS:
        if (you.jiyva_stat_targets[0] = JSTAT_UNSET)
            return ABIL_JIYVA_SET_TARGETS_FREE; // first time's free.
        return ability;

    case ABIL_LUGONU_ABYSS_EXIT:
    case ABIL_LUGONU_ABYSS_ENTER:
        if (brdepth[BRANCH_ABYSS] == -1)
            return ABIL_NON_ABILITY;
        return ability;

    case ABIL_TSO_BLESS_WEAPON:
    case ABIL_KIKU_BLESS_WEAPON:
    case ABIL_LUGONU_BLESS_WEAPON:
        if (you.species == SP_FELID || you.species == SP_FAIRY)
            return ABIL_NON_ABILITY;
        return ability;

    case ABIL_ELYVILON_HEAL_OTHER:
    case ABIL_TSO_SUMMON_DIVINE_WARRIOR:
    case ABIL_MAKHLEB_LESSER_SERVANT_OF_MAKHLEB:
    case ABIL_MAKHLEB_GREATER_SERVANT_OF_MAKHLEB:
    case ABIL_TROG_BROTHERS_IN_ARMS:
    case ABIL_GOZAG_BRIBE_BRANCH:
    case ABIL_QAZLAL_ELEMENTAL_FORCE:
        if (you.get_mutation_level(MUT_NO_LOVE))
            return ABIL_NON_ABILITY;
        return ability;

    case ABIL_KIKU_OPEN_CRYPTS:
        if (you.attribute[ATTR_KIKU_CORPSE])
            return ABIL_KIKU_CLOSE_CRYPTS;
        return ability;

    case ABIL_SIF_MUNA_DIVINE_ENERGY:
        if (you.attribute[ATTR_DIVINE_ENERGY])
            return ABIL_SIF_MUNA_STOP_DIVINE_ENERGY;
        return ability;

    case ABIL_ASHENZARI_TRANSFER_KNOWLEDGE:
        if (you.species == SP_GNOLL)
            return ABIL_NON_ABILITY;
        return ability;

    // Can only make the choices once.
    case ABIL_BAHAMUT_PROTECTION:
    case ABIL_TIAMAT_RETRIBUTION:
        if (you.props.exists(BAHAMUT_TIAMAT_CHOICE0_KEY))
            return ABIL_NON_ABILITY;
        return ability;

    case ABIL_CHOOSE_BAHAMUT_BREATH:
    case ABIL_CHOOSE_TIAMAT_BREATH:
        if (you.props.exists(BAHAMUT_TIAMAT_CHOICE1_KEY))
            return ABIL_NON_ABILITY;
        return ability;

    case ABIL_CHOOSE_BAHAMUT_DRAKE:
    case ABIL_CHOOSE_TIAMAT_DRAKE:
        if (you.props.exists(BAHAMUT_TIAMAT_CHOICE2_KEY))
            return ABIL_NON_ABILITY;
        return ability;

    case ABIL_BAHAMUT_TRANSFORM:
    case ABIL_CHOOSE_TIAMAT_TRANSFORM:
        if (you.experience_level < 7)
            return ABIL_NON_ABILITY; // Probably impossible anyways but immature draconians shouldn't unlock this yet.
        if (you.props.exists(BAHAMUT_TIAMAT_CHOICE3_KEY))
            return ABIL_NON_ABILITY;
        return ability;

    // You only have one of the choice abilities.
    case ABIL_BAHAMUT_EMPOWERED_BREATH:
        if (!you.props.exists(BAHAMUT_TIAMAT_CHOICE1_KEY))
            return ABIL_NON_ABILITY;
        if (!you.props[BAHAMUT_TIAMAT_CHOICE1_KEY].get_bool())
            return ABIL_NON_ABILITY;
        return ability;
    case ABIL_TIAMAT_ADAPTIVE_BREATH:
        if (!you.props.exists(BAHAMUT_TIAMAT_CHOICE1_KEY)) 
            return ABIL_NON_ABILITY;
        if (you.props[BAHAMUT_TIAMAT_CHOICE1_KEY].get_bool())
            return ABIL_NON_ABILITY;
        return ability;

    case ABIL_BAHAMUT_DRAKE_MOUNT:
        if (!you.props.exists(BAHAMUT_TIAMAT_CHOICE2_KEY))
            return ABIL_NON_ABILITY;
        if (!you.props[BAHAMUT_TIAMAT_CHOICE2_KEY].get_bool())
            return ABIL_NON_ABILITY;
        return ability;
    case ABIL_TIAMAT_SUMMON_DRAKES:
        if (!you.props.exists(BAHAMUT_TIAMAT_CHOICE2_KEY)) 
            return ABIL_NON_ABILITY;
        if (you.props[BAHAMUT_TIAMAT_CHOICE2_KEY].get_bool())
            return ABIL_NON_ABILITY;
        return ability;

    case ABIL_TIAMAT_TRANSFORM:
        if (!you.props.exists(BAHAMUT_TIAMAT_CHOICE3_KEY))
            return ABIL_NON_ABILITY;
        if (you.props[BAHAMUT_TIAMAT_CHOICE3_KEY].get_bool())
            return ABIL_NON_ABILITY;
        return ability;

    default:
        return ability;
    }
}

/// Handle special cases for ability failure chances.
static int _adjusted_failure_chance(ability_type ability, int base_chance)
{
    switch (ability)
    {
    case ABIL_BREATHE_DART:
    case ABIL_BREATHE_FIRE:
    case ABIL_BREATHE_FROST:
    case ABIL_BREATHE_ACID:
    case ABIL_BREATHE_LIGHTNING:
    case ABIL_BREATHE_POWER:
    case ABIL_BREATHE_MEPHITIC:
    case ABIL_BREATHE_STEAM:
    case ABIL_BREATHE_POISON:
    case ABIL_BREATHE_FOG:
    case ABIL_BREATHE_DRAIN:
    case ABIL_BREATHE_MIASMA:
    case ABIL_BREATHE_SILVER:
    case ABIL_BREATHE_WIND:
    case ABIL_BREATHE_BLOOD:
    case ABIL_BREATHE_HOLY_FLAMES:
    case ABIL_BREATHE_BUTTERFLIES:
    case ABIL_BREATHE_BONE:
    case ABIL_BREATHE_CHAOS:
    case ABIL_BREATHE_GHOSTLY_FLAMES:
    case ABIL_BREATHE_METAL:
    case ABIL_BREATHE_RADIATION:
    case ABIL_BREATHE_TRIPLE:
        if (you.form == transformation::dragon)
            return base_chance - 20;
        return base_chance;

    case ABIL_JIYVA_SLIMIFY:
        return max(0, base_chance - _slime_count());

    case ABIL_BLINK:
        return 48 - (17 * you.get_mutation_level(MUT_BLINK))
                  - you.experience_level / 2;
        break;

    case ABIL_NEMELEX_DEAL_FOUR:
        return 70 - (you.piety * 2 / 45) - you.skill(SK_INVOCATIONS, 9) / 2;

    default:
        return base_chance;
    }
}

talent get_talent(ability_type ability, bool check_confused)
{
    ASSERT(ability != ABIL_NON_ABILITY);

    // Placeholder handling, part 1: The ability we have might be a
    // placeholder, so convert it into its corresponding ability before
    // doing anything else, so that we'll handle its flags properly.
    talent result { fixup_ability(ability), 0, 0, false };
    const ability_def &abil = get_ability_def(result.which);

    if (check_confused && you.confused()
        && !testbits(abil.flags, abflag::conf_ok))
    {
        result.which = ABIL_NON_ABILITY;
        return result;
    }

    // Look through the table to see if there's a preference, else find
    // a new empty slot for this ability. - bwr
    const int index = find_ability_slot(abil.ability);
    result.hotkey = index >= 0 ? index_to_letter(index) : 0;

    const int base_chance = abil.failure.chance();
    const int failure = _adjusted_failure_chance(ability, base_chance);
    result.fail = max(0, min(100, failure));

    result.is_invocation = abil.failure.basis == fail_basis::invo;

    return result;
}

const char* ability_name(ability_type ability)
{
    return get_ability_def(ability).name;
}

vector<const char*> get_ability_names()
{
    vector<const char*> result;
    for (const talent &tal : your_talents(false))
        result.push_back(ability_name(tal.which));
    return result;
}

static string _desc_sac_mut(const CrawlStoreValue &mut_store)
{
    return mut_upgrade_summary(static_cast<mutation_type>(mut_store.get_int()));
}

static string _sacrifice_desc(const ability_type ability)
{
    const string boilerplate =
        "\nIf you make this sacrifice, your powers granted by Ru "
        "will become stronger in proportion to the value of the "
        "sacrifice, and you may gain new powers as well.\n\n"
        "Sacrifices cannot be taken back.\n";
    const string piety_info = ru_sacrifice_description(ability);
    const string desc = boilerplate + piety_info;

    if (!you_worship(GOD_RU))
        return desc;

    const string sac_vec_key = ru_sacrifice_vector(ability);
    if (sac_vec_key.empty())
        return desc;

    ASSERT(you.props.exists(sac_vec_key));
    const CrawlVector &sacrifice_muts = you.props[sac_vec_key].get_vector();
    return "\nAfter this sacrifice, you will find that "
            + comma_separated_fn(sacrifice_muts.begin(), sacrifice_muts.end(),
                                 _desc_sac_mut)
            + ".\n" + desc;
}

// XXX: should this be in describe.cc?
string get_ability_desc(const ability_type ability, bool need_title)
{
    const string& name = ability_name(ability);

    string lookup = getLongDescription(name + " ability");

    if (lookup.empty()) // Nothing found?
        lookup = "No description found.\n";

    if (testbits(get_ability_def(ability).flags, abflag::sacrifice))
        lookup += _sacrifice_desc(ability);

    if (god_hates_ability(ability, you.religion))
    {
        lookup += uppercase_first(god_name(you.religion))
                  + " frowns upon the use of this ability.\n";
    }

    ostringstream res;
    if (need_title)
        res << name << "\n\n";
    res << lookup << "\n" << _detailed_cost_description(ability);

    const string quote = getQuoteString(name + " ability");
    if (!quote.empty())
        res << "\n\n" << quote;

    return res.str();
}

static void _print_talent_description(const talent& tal)
{
    describe_ability(tal.which);
}

void no_ability_msg()
{
    // Give messages if the character cannot use innate talents right now.
    // * Tengu can't start to fly if already flying.
    if (you.get_mutation_level(MUT_TENGU_FLIGHT)
             || you.get_mutation_level(MUT_BIG_WINGS))
    {
        if (you.airborne())
            mpr("You're already flying!");
    }
    else
        mpr("Sorry, you're not good enough to have a special ability.");
}

bool activate_ability()
{
    vector<talent> talents = your_talents(false);

    if (talents.empty())
    {
        no_ability_msg();
        crawl_state.zero_turns_taken();
        return false;
    }

    int selected = -1;
#ifndef TOUCH_UI
    if (Options.ability_menu)
#endif
    {
        selected = choose_ability_menu(talents);
        if (selected == -1)
        {
            canned_msg(MSG_OK);
            crawl_state.zero_turns_taken();
            return false;
        }
    }
#ifndef TOUCH_UI
    else
    {
        while (selected < 0)
        {
            msg::streams(MSGCH_PROMPT) << "Use which ability? (? or * to list) "
                                       << endl;

            const int keyin = get_ch();

            if (keyin == '?' || keyin == '*')
            {
                selected = choose_ability_menu(talents);
                if (selected == -1)
                {
                    canned_msg(MSG_OK);
                    crawl_state.zero_turns_taken();
                    return false;
                }
            }
            else if (key_is_escape(keyin) || keyin == ' ' || keyin == '\r'
                     || keyin == '\n')
            {
                canned_msg(MSG_OK);
                crawl_state.zero_turns_taken();
                return false;
            }
            else if (isaalpha(keyin))
            {
                // Try to find the hotkey.
                for (unsigned int i = 0; i < talents.size(); ++i)
                {
                    if (talents[i].hotkey == keyin)
                    {
                        selected = static_cast<int>(i);
                        break;
                    }
                }

                // If we can't, cancel out.
                if (selected < 0)
                {
                    mpr("You can't do that.");
                    crawl_state.zero_turns_taken();
                    return false;
                }
            }
        }
    }
#endif
    return activate_talent(talents[selected]);
}

static bool _can_hop(bool quiet)
{
    if (you.duration[DUR_NO_HOP])
    {
        if (!quiet)
            mpr("Your legs are too worn out to hop.");
        return false;
    }
    if (you.mounted())
    {
        if (!quiet)
            mpr("You cannot hop off of your mount.");
        return false;
    }
    if (!form_keeps_mutations())
    {
        if (!quiet)
            mpr("You cannot hop in your current form.");
        return false;
    }
    return true;
}

static bool _can_jump(bool quiet, bool jump)
{
    if (!you.duration[DUR_MOUNT_BREATH] && !you.duration[DUR_ENSNARE])
        return true;
    if (!quiet)
    {
        if (you.duration[DUR_MOUNT_BREATH])
            mpr("Your spider is still catching its breath.");
        if (you.duration[DUR_ENSNARE])
        {
            if (jump)
                mpr("Your spider can't jump while it's legs are covered in web.");
            else
                mpr("Your spider already prepped a web.");
        }
    }
    return false;
}

// Check prerequisites for a number of abilities.
// Abort any attempt if these cannot be met, without losing the turn.
// TODO: Many more cases need to be added!
static bool _check_ability_possible(const ability_def& abil, bool quiet = false)
{
    if (you.berserk() && 
        !(testbits(abil.flags, abflag::berserk_ok) || testbits(abil.flags, abflag::berserk_only)))
    {
        if (!quiet)
            canned_msg(MSG_TOO_BERSERK);
        return false;
    }

    if (!you.berserk() && testbits(abil.flags, abflag::berserk_only))
    {
        if (!quiet)
            simple_god_message(" only hears the pleas of the enraged!");
        return false;
    }

    string local_prompt = "";

    // Doing these would outright kill the player.
    // (or, in the case of the stat-zeros, they'd at least be extremely
    // dangerous.)
    if (abil.ability == ABIL_STOP_FLYING)
    {
        if (quiet)
            return false;
        if (is_feat_dangerous(grd(you.pos()), false, true))
        {
            local_prompt = make_stringf("Stopping flight right now would cause you to %s! Are you sure you want to stop flying?",
                env.grid(you.pos()) == DNGN_LAVA ? "burn" : "drown");

            return yesno(local_prompt.c_str(), true, 'n');
        }
    }
    else if (abil.ability == ABIL_END_TRANSFORMATION)
    {
        if (quiet)
            return false;
        if (feat_dangerous_for_form(transformation::none, env.grid(you.pos())))
        {
            local_prompt = make_stringf("Turning back now would cause you to %s! Are you sure you want end your transformation?",
                env.grid(you.pos()) == DNGN_LAVA ? "burn" : "drown");

            return yesno(local_prompt.c_str(), true, 'n');
        }
    }

    if ((abil.ability == ABIL_EVOKE_BERSERK
         || abil.ability == ABIL_TROG_BERSERK)
        && !you.can_go_berserk(true, false, quiet))
    {
        return false;
    }

    if ((abil.ability == ABIL_EVOKE_FLIGHT
         || abil.ability == ABIL_TRAN_BAT
         || abil.ability == ABIL_FLY)
        && !flight_allowed(quiet))
    {
        return false;
    }

    if (you.confused() && !testbits(abil.flags, abflag::conf_ok))
    {
        if (!quiet)
            canned_msg(MSG_TOO_CONFUSED);
        return false;
    }

    // Silence and water elementals
    if (silenced(you.pos()) && !you.can_silent_cast()
        || you.duration[DUR_WATER_HOLD] && !you.res_water_drowning())
    {
        talent tal = get_talent(abil.ability, false);
        if (tal.is_invocation && !testbits(abil.flags, abflag::silence_ok))
        {
            if (!quiet)
            {
                mprf("You cannot call out to %s while %s.",
                     god_name(you.religion).c_str(),
                     silenced(you.pos())          ? "silenced"
                                                  : "unable to breathe");
            }
            return false;
        }
    }

    if (!testbits(abil.flags, abflag::starve_ok)
        && apply_starvation_penalties())
    {
        if (!quiet)
            canned_msg(MSG_TOO_HUNGRY);
        return false;
    }

    // Don't insta-starve the player.
    // (Losing consciousness possible from 400 downward.)
    if (!testbits(abil.flags, abflag::starve_ok) && !you.undead_state())
    {
        const hunger_state_t state =
            static_cast<hunger_state_t>(max(0, you.hunger_state - 1));
        const int expected_hunger = hunger_threshold[state]
                                    - div_round_up(abil.food_cost * 6, 5);
        if (!quiet)
        {
            dprf("hunger: %d, max. food_cost: %d, expected hunger: %d",
                 you.hunger, abil.food_cost * 2, expected_hunger);
        }
        // Safety margin for natural hunger, mutations etc.
        if (expected_hunger <= 50)
        {
            if (!quiet)
                canned_msg(MSG_TOO_HUNGRY);
            return false;
        }
    }

    const god_power* god_power = god_power_from_ability(abil.ability);
    if (god_power && !god_power_usable(*god_power))
    {
        if (!quiet)
            canned_msg(MSG_GOD_DECLINES);
        return false;
    }

    if (!quiet)
    {
        vector<text_pattern> &actions = Options.confirm_action;
        if (!actions.empty())
        {
            const char* name = ability_name(abil.ability);
            for (const text_pattern &action : actions)
            {
                if (action.matches(name))
                {
                    string prompt = "Really use " + string(name) + "?";
                    if (!yesno(prompt.c_str(), false, 'n'))
                    {
                        canned_msg(MSG_OK);
                        return false;
                    }
                    break;
                }
            }
        }
    }

    // Check that we can afford to pay the costs.
    // Note that mutation shenanigans might leave us with negative MP,
    // so don't fail in that case if there's no MP cost.
    if (ability_mp_cost(abil.ability) > 0 && !enough_mp(ability_mp_cost(abil.ability), quiet, true))
        return false;

    const int hpcost = abil.hp_cost.cost(you.hp_max);
    if (hpcost > 0 && !enough_hp(hpcost, quiet))
        return false;

    switch (abil.ability)
    {
    case ABIL_ZIN_RECITE:
    {
        if (!zin_check_able_to_recite(quiet))
            return false;

        int result = zin_check_recite_to_monsters(quiet);
        if (result != 1)
        {
            if (!quiet)
            {
                if (result == 0)
                    mpr("There's no appreciative audience!");
                else if (result == -1)
                    mpr("You are not zealous enough to affect this audience!");
            }
            return false;
        }
        return true;
    }

    case ABIL_ZIN_SANCTUARY:
        if (env.sanctuary_time)
        {
            if (!quiet)
                mpr("There's already a sanctuary in place on this level.");
            return false;
        }
        return true;

    case ABIL_ZIN_DONATE_GOLD:
        if (!you.gold)
        {
            if (!quiet)
                mpr("You have nothing to donate!");
            return false;
        }
        return true;

    case ABIL_ELYVILON_PURIFICATION:
        if (!you.disease && !you.duration[DUR_POISONING]
            && !you.duration[DUR_CONF] && !you.duration[DUR_SLOW]
            && !you.petrifying()
            && you.strength(false) == you.max_strength()
            && you.intel(false) == you.max_intel()
            && you.dex(false) == you.max_dex()
            && !player_rotted()
            && !you.duration[DUR_WEAK])
        {
            if (!quiet)
                mpr("Nothing ails you!");
            return false;
        }
        return true;

    case ABIL_LUGONU_ABYSS_EXIT:
        if (!player_in_branch(BRANCH_ABYSS))
        {
            if (!quiet)
                mpr("You aren't in the Abyss!");
            return false;
        }
        return true;

    case ABIL_LUGONU_CORRUPT:
        return !is_level_incorruptible(quiet);

    case ABIL_LUGONU_ABYSS_ENTER:
        if (player_in_branch(BRANCH_ABYSS))
        {
            if (!quiet)
                mpr("You're already here!");
            return false;
        }
        return true;

    case ABIL_SIF_MUNA_FORGET_SPELL:
        if (you.spell_no == 0)
        {
            if (!quiet)
                canned_msg(MSG_NO_SPELLS);
            return false;
        }
        return true;

    case ABIL_ASHENZARI_TRANSFER_KNOWLEDGE:
        if (!trainable_skills(true))
        {
            if (!quiet)
                mpr("You have nothing more to learn.");
            return false;
        }
        return true;

    case ABIL_FEDHAS_EVOLUTION:
        return fedhas_check_evolve_flora(quiet);

    case ABIL_FEDHAS_SPAWN_SPORES:
    {
        const int retval = fedhas_check_corpse_spores(quiet);
        if (retval <= 0)
        {
            if (!quiet)
            {
                if (retval == 0)
                    mpr("No corpses are in range.");
                else
                    canned_msg(MSG_OK);
            }
            return false;
        }
        return true;
    }

    case ABIL_JIYVA_SLIMIFY:
    {
        if (_slime_count() < 6)
        {
            if (!quiet)
                mpr("Not enough fresh slime nearby.");
            return false;
        }

        return true;
    }

    case ABIL_BREATHE_MAGMA:
    case ABIL_SPIT_POISON:
    case ABIL_BREATHE_DART:
    case ABIL_BREATHE_FIRE:
    case ABIL_BREATHE_FROST:
    case ABIL_BREATHE_ACID:
    case ABIL_BREATHE_LIGHTNING:
    case ABIL_BREATHE_POWER:
    case ABIL_BREATHE_MEPHITIC:
    case ABIL_BREATHE_STEAM:
    case ABIL_BREATHE_POISON:
    case ABIL_BREATHE_FOG:
    case ABIL_BREATHE_DRAIN:
    case ABIL_BREATHE_MIASMA:
    case ABIL_BREATHE_SILVER:
    case ABIL_BREATHE_WIND:
    case ABIL_BREATHE_BLOOD:
    case ABIL_BREATHE_HOLY_FLAMES:
    case ABIL_BREATHE_BUTTERFLIES:
    case ABIL_BREATHE_BONE:
    case ABIL_BREATHE_CHAOS:
    case ABIL_BREATHE_GHOSTLY_FLAMES:
    case ABIL_BREATHE_METAL:
    case ABIL_BREATHE_RADIATION:
    case ABIL_BAHAMUT_EMPOWERED_BREATH:
    case ABIL_TIAMAT_ADAPTIVE_BREATH:
        if (you.duration[DUR_BREATH_WEAPON])
        {
            if (!quiet)
                canned_msg(MSG_CANNOT_DO_YET);
            return false;
        }

        if (!you.is_unbreathing() && you.res_poison() < 2
            && cloud_at(you.pos()) && cloud_at(you.pos())->type == CLOUD_MEPHITIC
            && one_chance_in(1 + div_round_up(you.experience_level, 8)))
        {
            mpr("You sharply inhale and choke on fumes!");
            return false;
        }
        return true;

    case ABIL_HEAL_WOUNDS:
        if (you.hp == you.hp_max)
        {
            if (!quiet)
                canned_msg(MSG_FULL_HEALTH);
            return false;
        }
        if (get_real_mp(false) < 1)
        {
            if (!quiet)
                mpr("You don't have enough innate magic capacity.");
            return false;
        }
        return true;

    case ABIL_SHAFT_SELF:
        return you.can_do_shaft_ability(quiet);

    case ABIL_HOP:
        return _can_hop(quiet);

    case ABIL_SPIDER_WEB:
    case ABIL_SPIDER_JUMP:
        return _can_jump(quiet, abil.ability == ABIL_SPIDER_JUMP);

    case ABIL_BLINK:
    case ABIL_EVOKE_BLINK:
    {
        const string no_tele_reason = you.no_tele_reason(false, true);
        if (no_tele_reason.empty())
            return true;

        if (!quiet)
             mpr(no_tele_reason);
        return false;
    }

    case ABIL_EVOKE_BERSERK:
    case ABIL_TROG_BERSERK:
        return you.can_go_berserk(true, false, true)
               && (quiet || berserk_check_wielded_weapon());

    case ABIL_EVOKE_FOG:
        if (cloud_at(you.pos()))
        {
            if (!quiet)
                mpr("It's too cloudy to do that here.");
            return false;
        }
        if (env.level_state & LSTATE_STILL_WINDS)
        {
            if (!quiet)
                mpr("The air is too still for clouds to form.");
            return false;
        }
        return true;

    case ABIL_GOZAG_POTION_PETITION:
        return gozag_setup_potion_petition(quiet);

    case ABIL_GOZAG_CALL_MERCHANT:
        return gozag_setup_call_merchant(quiet);

    case ABIL_GOZAG_BRIBE_BRANCH:
        return gozag_check_bribe_branch(quiet);

    case ABIL_RU_SACRIFICE_EXPERIENCE:
        if (you.experience_level <= RU_SAC_XP_LEVELS)
        {
            if (!quiet)
                mpr("You don't have enough experience to sacrifice.");
            return false;
        }
        return true;

#if TAG_MAJOR_VERSION == 34
    case ABIL_PAKELLAS_DEVICE_SURGE:
        if (you.magic_points == 0)
        {
            if (!quiet)
                mpr("You have no magic power.");
            return false;
        }
        return true;
#endif

        // only available while your ancestor is alive.
    case ABIL_HEPLIAKLQANA_IDEALISE:
    case ABIL_HEPLIAKLQANA_RECALL:
    case ABIL_HEPLIAKLQANA_TRANSFERENCE:
        if (hepliaklqana_ancestor() == MID_NOBODY)
        {
            if (!quiet)
            {
                mprf("%s is still trapped in memory!",
                     hepliaklqana_ally_name().c_str());
            }
            return false;
        }
        return true;

    case ABIL_WU_JIAN_WALLJUMP:
    {
        // TODO: Add check for whether there is any valid landing spot
        if (you.is_nervous())
        {
            if (!quiet)
                mpr("You are too terrified to wall jump!");
            return false;
        }
        if (you.attribute[ATTR_HELD])
        {
            if (!quiet)
            {
                mprf("You cannot wall jump while caught in a %s.",
                     get_trapping_net(you.pos()) == NON_ITEM ? "web" : "net");
            }
            return false;
        }
        // Is there a valid place to wall jump?
        bool has_targets = false;
        for (adjacent_iterator ai(you.pos()); ai; ++ai)
            if (feat_can_wall_jump_against(grd(*ai)))
            {
                has_targets = true;
                break;
            }

        if (!has_targets)
        {
            if (!quiet)
                mpr("There is nothing to wall jump against here.");
            return false;
        }
        return true;
    }

    default:
        return true;
    }
}

static bool _check_ability_dangerous(const ability_type ability,
                                     bool quiet = false)
{
    if (ability == ABIL_TRAN_BAT)
        return !check_form_stat_safety(transformation::bat, quiet);
    else if (ability == ABIL_END_TRANSFORMATION
             && !feat_dangerous_for_form(transformation::none,
                                         env.grid(you.pos())))
    {
        return !check_form_stat_safety(transformation::bat, quiet);
    }
    else
        return false;
}

bool check_ability_possible(const ability_type ability, bool quiet)
{
    return _check_ability_possible(get_ability_def(ability), quiet);
}

bool activate_talent(const talent& tal)
{
    const ability_def& abil = get_ability_def(tal.which);

    if (_check_ability_dangerous(abil.ability) || !_check_ability_possible(abil))
    {
        crawl_state.zero_turns_taken();
        return false;
    }

    bool fail = random2avg(100, 3) < tal.fail;

    const spret ability_result = _do_ability(abil, fail);
    switch (ability_result)
    {
        case spret::success:
            ASSERT(!fail || testbits(abil.flags, abflag::hostile));
            practise_using_ability(abil.ability);
            _pay_ability_costs(abil);
            count_action(tal.is_invocation ? CACT_INVOKE : CACT_ABIL, abil.ability);
            return true;
        case spret::fail:
            mpr("You fail to use your ability.");
            you.turn_is_over = true;
            return false;
        case spret::abort:
            crawl_state.zero_turns_taken();
            return false;
        case spret::none:
        default:
            die("Weird ability return type");
            return false;
    }
}

static int _calc_breath_ability_range(ability_type ability)
{
    int range = 0;

    switch (ability)
    {
    case ABIL_BREATHE_BONE:
    case ABIL_BREATHE_SILVER:
    case ABIL_BREATHE_METAL:
    case ABIL_BREATHE_ACID:
    case ABIL_BREATHE_RADIATION:
    case ABIL_BREATHE_MIASMA:
    case ABIL_BREATHE_DRAIN:
        range = 3;
        break;
    case ABIL_BREATHE_BLOOD:
    case ABIL_BREATHE_GHOSTLY_FLAMES:
    case ABIL_BREATHE_FIRE:
    case ABIL_BREATHE_FROST:
    case ABIL_BREATHE_DART:
    case ABIL_BREATHE_MEPHITIC:
    case ABIL_BREATHE_POISON:
    case ABIL_BREATHE_MAGMA:
        range = 4;
        break;
    case ABIL_BREATHE_BUTTERFLIES:
    case ABIL_BREATHE_HOLY_FLAMES:
    case ABIL_SPIT_POISON:
    case ABIL_BREATHE_TRIPLE:
        range = 5;
        break;
    case ABIL_BREATHE_STEAM:
    case ABIL_BREATHE_FOG:
        range = 6;
        break;
    case ABIL_BREATHE_WIND:
    case ABIL_BREATHE_CHAOS:
    case ABIL_BREATHE_LIGHTNING:
    case ABIL_BREATHE_POWER:
        range = LOS_MAX_RANGE;
    default:    break;
    }

    return min((int)you.current_vision, range);
}

static bool _acid_breath_can_hit(const actor *act)
{
    if (act->is_monster())
    {
        const monster* mons = act->as_monster();
        bolt testbeam;
        testbeam.thrower = KILL_YOU;
        zappy(ZAP_BREATHE_ACID, 100, false, testbeam);

        return !testbeam.ignores_monster(mons);
    }
    else
        return false;
}

/// If the player is stationary, print 'You cannot move.' and return true.
static bool _abort_if_stationary()
{
    if (!you.is_stationary())
        return false;

    canned_msg(MSG_CANNOT_MOVE);
    return true;
}

static bool _cleansing_flame_affects(const actor *act)
{
    return act->res_holy_energy() < 3;
}

bool previously_on = false;

// This is a short circuit used by Tiamat's breath ability.
spret tiamat_breath(const ability_type abil, const bool bahamut)
{
    const ability_def& ability = get_ability_def(abil);

    return _do_ability(ability, false, bahamut);
}

static int _pois_res_multi(monster * mons)
{
    switch (mons->res_poison())
    {
    default:
    case -1:
        return 5;
    case 0:
        return 10;
    case 1:
        return 20;
    case 2:
        return 25;
    case 3:
        return 1000; // Arbitrarily high to be completely impossible to succeed.
    }
}

static void _spawn_eyeballs()
{
    const int power = you.experience_level + you.skill(SK_INVOCATIONS);
    int sumcount = div_rand_round(power, 9);
    sumcount += 1 + random2(sumcount);

    for (int i = 0; i < sumcount; i++)
    {
        const monster_type mon = random_choose_weighted(
             2, MONS_FLOATING_EYE,
             1, MONS_GOLDEN_EYE,
             1, MONS_SHINING_EYE,
             4, MONS_EYE_OF_DEVASTATION);

        monster * x = create_monster(
            mgen_data(mon, BEH_FRIENDLY, you.pos(), MHITNOT, MG_NONE, GOD_JIYVA));
        if (x)
            x->add_ench(mon_enchant(ENCH_FAKE_ABJURATION, 5));

        for (adjacent_iterator ai(you.pos()); ai; ++ai)
        {
            if (!actor_at(*ai))
            {
                x->move_to_pos(*ai, true, true);
                break;
            }
        }
    }
}

/*
 * Use an ability.
 *
 * @param abil The actual ability used.
 * @param fail If true, the ability is doomed to fail, and spret::fail will
 * be returned if the ability is not spret::aborted.
 * @param empowered used by Bahamut's empowered breath ability; causes the 
 * called breath ability to be strongr than normal.
 * @returns Whether the spell succeeded (spret::success), failed (spret::fail),
 *  or was canceled (spret::abort). Never returns spret::none.
 */
static spret _do_ability(const ability_def& abil, bool fail, bool empowered)
{
    dist abild;
    bolt beam;
    dist spd;

    if (int(div_round_up(abil.food_cost * 6, 5) + HUNGER_FAINTING + 10) >= you.hunger)
    {
        mprf(MSGCH_WARN, "If you use your ability, you could pass out from exhaustion!");

        if (!yesno("Continue?", true, 0))
        {
            canned_msg(MSG_OK);
            return spret::abort;
        }
    }

    // Note: the costs will not be applied until after this switch
    // statement... it's assumed that only failures have returned! - bwr
    switch (abil.ability)
    {
    case ABIL_HEAL_WOUNDS:
        fail_check();
        if (one_chance_in(4))
        {
            mpr("Your magical essence is drained by the effort!");
            rot_mp(1);
        }
        potionlike_effect(POT_HEAL_WOUNDS, 40);
        break;

    case ABIL_DIG:
        fail_check();
        if (!you.digging)
        {
            you.digging = true;
            mpr("You extend your mandibles.");
        }
        else
        {
            you.digging = false;
            mpr("You retract your mandibles.");
        }
        break;

    case ABIL_SHAFT_SELF:
        fail_check();
        if (you.can_do_shaft_ability(false))
        {
            if (yesno("Are you sure you want to shaft yourself?", true, 'n'))
                start_delay<ShaftSelfDelay>(1);
            else
                return spret::abort;
        }
        else
            return spret::abort;
        break;

    case ABIL_HOP:
        if (_can_hop(false))
            return frog_hop(fail);
        else
            return spret::abort;

    case ABIL_SPIDER_JUMP:
        if (_can_jump(false, true))
            return frog_hop(fail, true);
        else
            return spret::abort;

    case ABIL_SPIDER_WEB:
        if (_can_jump(false, false))
        {
            fail_check();
            you.set_duration(DUR_ENSNARE, 3 + random2(calc_spell_power(SPELL_SUMMON_SPIDER_MOUNT, true) / 20));
            mprf(MSGCH_DURATION, "Your spider prepares a web to ensnare its next melee target.");
            return spret::success;
        }
        else
            return spret::abort;

    case ABIL_SPIT_POISON:      // Naga poison spit
    {
        int power = 10 + you.experience_level;
        beam.range = _calc_breath_ability_range(abil.ability);

        if (!spell_direction(abild, beam)
            || !player_tracer(ZAP_SPIT_POISON, power, beam))
        {
            return spret::abort;
        }
        else
        {
            fail_check();
            zapping(ZAP_SPIT_POISON, power, beam);
            you.set_duration(DUR_BREATH_WEAPON, 3 + random2(5));
        }
        break;
    }

    // Shotgun Breaths
    case ABIL_BREATHE_METAL:
    case ABIL_BREATHE_SILVER:
    case ABIL_BREATHE_BONE:
    {
        beam.range = _calc_breath_ability_range(abil.ability);

        zap_type zap;

        switch (abil.ability)
        {
        default:
        case ABIL_BREATHE_BONE:
            zap = ZAP_BREATHE_BONE;
            break;
        case ABIL_BREATHE_SILVER:
            zap = ZAP_BREATHE_SILVER;
            break;
        case ABIL_BREATHE_METAL:
            zap = ZAP_BREATHE_METAL;
            break;
        }

        const int power = drac_breath_power(empowered);

        targeter_shotgun hitfunc(&you, shotgun_beam_count(power), beam.range);
        direction_chooser_args args;
        args.mode = TARG_HOSTILE;
        args.hitfunc = &hitfunc;
        args.self = confirm_prompt_type::cancel;
        if (!spell_direction(abild, beam, &args))
            return spret::abort;

        fail_check();

        spret s = cast_scattershot(&you, power, beam.target, false, zap, empowered);

        if (s == spret::success)
            you.increase_duration(DUR_BREATH_WEAPON,
                5 + random2(15) + random2(40 - you.experience_level));

        return s;
    }

    // Cloud Cone Breaths
    case ABIL_BREATHE_MEPHITIC:
    case ABIL_BREATHE_POISON:
    case ABIL_BREATHE_FOG:
    case ABIL_BREATHE_BLOOD:
    case ABIL_BREATHE_GHOSTLY_FLAMES:
    {
        cloud_type cloud;

        switch (abil.ability)
        {
        case ABIL_BREATHE_GHOSTLY_FLAMES:   cloud = CLOUD_SPECTRAL;         break;
        case ABIL_BREATHE_FOG:              cloud = CLOUD_PURPLE_SMOKE;     break;
        case ABIL_BREATHE_POISON:           cloud = CLOUD_POISON;           break;
        case ABIL_BREATHE_BLOOD:            cloud = CLOUD_BLOOD;            break;
        case ABIL_BREATHE_MEPHITIC:         empowered   ?   cloud = CLOUD_POISON
                                                        :   cloud = CLOUD_MEPHITIC;
                                                                            break;
        default:                            cloud = CLOUD_CHAOS;            break;  
        }

        const int range = _calc_breath_ability_range(abil.ability);
        targeter_shotgun hitfunc(&you, CLOUD_CONE_BEAM_COUNT, range);

        direction_chooser_args args;
        args.mode = TARG_HOSTILE;
        args.hitfunc = &hitfunc;
        args.top_prompt = "Breath at?";
        args.self = confirm_prompt_type::cancel;

        if (!spell_direction(abild, beam, &args))
            return spret::abort;

        fail_check();

        int power = drac_breath_power(empowered);

        if (abil.ability == ABIL_BREATHE_FOG)
        {
            if (empowered)
                mass_enchantment(ENCH_FEAR, power * 5, false);
            mpr("You exhale a massive amount of fog.");
        }
        else
        {
            mprf("You exhale a mighty wave of %s!",
                cloud_type_name(cloud).c_str());
        }

        for (const auto &entry : hitfunc.zapped)
        {
            if (entry.second <= 0)
                continue;
            if (empowered && monster_at(entry.first))
            {
                monster * mons = monster_at(entry.first);
                if (cloud == CLOUD_BLOOD 
                    && x_chance_in_y(power, mons->get_experience_level() * 2)
                    && !x_chance_in_y(mons->res_negative_energy(), 3))
                {
                    mprf("You drain %s vigour!",
                        mons->name(DESC_ITS).c_str());
                    mons->slow_down(&you, power/5 + random2(power));
                }
                else if (cloud == CLOUD_POISON && x_chance_in_y(roll_dice(2, power), 
                    mons->get_experience_level() * _pois_res_multi(mons) / 10))
                {
                    simple_monster_message(*mons, " chokes on the fumes.");
                    mons->add_ench(mon_enchant(ENCH_CONFUSION, 0, &you, 
                        (power/5 + random2(power)) * BASELINE_DELAY));
                }
                else if (cloud == CLOUD_SPECTRAL)
                {
                    int dam = 2 + div_rand_round(power, 6);
                    dam = roll_dice(3, dam);

                    if (mons->holiness() & MH_UNDEAD)
                        dam = 0 - dam / 3;
                    else
                        dam = resist_adjust_damage(mons, BEAM_NEG, dam);

                    if (dam < 0)
                    {
                        mons->heal(dam, true);
                        mprf("%s is healed by the spectral mist%s", mons->name(DESC_THE).c_str(), attack_strength_punctuation(abs(dam)).c_str());
                    }

                    else
                    {
                        mons->hurt(&you, dam, BEAM_NEG, KILLED_BY_DRAINING);
                        if (dam && mons->alive())
                            mons->drain_exp(&you);
                    }
                }
            }
            place_cloud(cloud, entry.first,
                max(5, random2avg(power, 3)),
                &you, div_round_up(power, 10) - 1);
        }

        mount_drake_breath(&beam);

        you.increase_duration(DUR_BREATH_WEAPON,
            3 + random2(10) + random2(30 - you.experience_level));
        break;
    }

    // Only breath with a "splash" effect.
    case ABIL_BREATHE_ACID:       // Draconian acid splash
    {
        beam.range = _calc_breath_ability_range(abil.ability);
        targeter_splash hitfunc(&you, beam.range);
        direction_chooser_args args;
        args.mode = TARG_HOSTILE;
        args.hitfunc = &hitfunc;
        args.top_prompt = "Spit at?";
        args.self = confirm_prompt_type::cancel;

        if (!spell_direction(abild, beam, &args))
          return spret::abort;

        if (stop_attack_prompt(hitfunc, "spit at", _acid_breath_can_hit))
          return spret::abort;

        fail_check();

        if (empowered)
            beam.origin_spell = SPELL_EMPOWERED_BREATH;

        zapping(ZAP_BREATHE_ACID, drac_breath_power(empowered),
                beam, false, "You spit a glob of acid.");

        mount_drake_breath(&beam);

        you.increase_duration(DUR_BREATH_WEAPON,
                          3 + random2(10) + random2(30 - you.experience_level));
        break;
    }

    // Blast Breaths
    case ABIL_BREATHE_LIGHTNING:
    {
        targeter_radius hitfunc(&you, LOS_NO_TRANS, 2, 0, 2);

        auto vulnerable = [](const actor *act) -> bool
        {
            return !(you.deity() == GOD_FEDHAS
                    && fedhas_protects(act->as_monster()))
                && (act->res_elec() < 3);
        };

        if (stop_attack_prompt(hitfunc, "lightning breath", vulnerable))
            return spret::abort;

        fail_check();

        bolt vis_beam;
        vis_beam.name = "lightning breath";
        vis_beam.flavour = BEAM_VISUAL;
        vis_beam.set_agent(&you);
        vis_beam.colour = CYAN;
        vis_beam.glyph = dchar_glyph(DCHAR_EXPLOSION);
        vis_beam.range = 1;
        vis_beam.ex_size = 2;
        vis_beam.is_explosion = true;
        vis_beam.explode_delay = beam.explode_delay * 3 / 2;
        vis_beam.source = you.pos();
        vis_beam.target = you.pos();
        vis_beam.hit = AUTOMATIC_HIT;
        vis_beam.loudness = 0;
        vis_beam.explode(true, true);

        mpr("You breathe a wild blast of lightning!");

        bolt dam_beam;
        int power = drac_breath_power(empowered);
        zappy(ZAP_BREATHE_LIGHTNING, power, false, dam_beam);

        for (radius_iterator ri(you.pos(), 2, C_SQUARE, true); ri; ++ri)
        {
            actor *act = actor_at(*ri);
            if (act && act->alive())
            {
                if (you.religion == GOD_FEDHAS && fedhas_protects(act->as_monster()))
                    simple_god_message(" protects your plant from harm.", GOD_FEDHAS);
                else
                {
                    dam_beam.target = *ri;
                    dam_beam.in_explosion_phase = true;
                    dam_beam.explosion_affect_cell(*ri);
                }
            }
            if (empowered && !cell_is_solid(*ri))
            {
                if (x_chance_in_y(power, 120))
                    place_cloud(CLOUD_STORM, *ri, 8 + random2avg(power / 3, 2), &you, 2);
                else if (x_chance_in_y(power, 90))
                    place_cloud(CLOUD_RAIN, *ri, 8 + random2avg(power / 2, 2), &you, 1);
            }
        }

        you.increase_duration(DUR_BREATH_WEAPON,
            3 + random2(10) + random2(30 - you.experience_level));
        break;
    }

    case ABIL_BREATHE_WIND:
    {
        fail_check();

        int power = drac_breath_power(empowered);

        if (empowered)
        {
            spret local = fire_los_attack_spell(SPELL_EMPOWERED_BREATH, power, &you, nullptr, false);
            if (local == spret::abort)
                return spret::abort;
        }

        wind_blast(&you, power * 5, coord_def(), empowered ? 3 : 2);

        you.increase_duration(DUR_BREATH_WEAPON,
            3 + random2(10) + random2(30 - you.experience_level));
        break;
    }

    // Bolt/Shard Breaths.
    case ABIL_BREATHE_DART:
    case ABIL_BREATHE_FIRE:
    case ABIL_BREATHE_FROST:
    case ABIL_BREATHE_POWER:
    case ABIL_BREATHE_STEAM:
    case ABIL_BREATHE_DRAIN:
    case ABIL_BREATHE_MIASMA:
    case ABIL_BREATHE_HOLY_FLAMES:
    case ABIL_BREATHE_BUTTERFLIES:
    case ABIL_BREATHE_CHAOS:
    case ABIL_BREATHE_RADIATION:
    case ABIL_BREATHE_TRIPLE:
    case ABIL_BREATHE_MAGMA:
    {
        beam.range = _calc_breath_ability_range(abil.ability);
        
        direction_chooser_args args;
        args.top_prompt = "Breath at?";
        args.mode = TARG_HOSTILE;

        if (abil.ability != ABIL_BREATHE_POWER)
            args.self = confirm_prompt_type::cancel;

        if (!spell_direction(abild, beam, &args))
            return spret::abort;

        string m;
        zap_type zap;
        const int power = drac_breath_power(empowered);

        fail_check();

        if (empowered && abil.ability == ABIL_BREATHE_HOLY_FLAMES)
        {
            targeter_radius hitfunc(&you, LOS_NO_TRANS);

            if (stop_attack_prompt(hitfunc, "let out a sacred roar",
                [](const actor* monpopo)
            {
                return monpopo->undead_or_demonic();
            },
                nullptr, nullptr))
            {
                return spret::abort;
            }

            holy_word(power * 5, HOLY_WORD_BREATH, you.pos(), false, &you);
        }

        switch (abil.ability)
        {
        case ABIL_BREATHE_TRIPLE:
            zap = ZAP_BREATHE_TRIPLE;
            m   = "You expend all your breath powers at once!";
            break;

        case ABIL_BREATHE_MAGMA:
            if (you.get_mutation_level(MUT_BREATHE_MAGMA) > 1)
                zap = ZAP_BREATHE_MAGMA_II;
            else
                zap = ZAP_BREATHE_MAGMA;
            m   = make_stringf("You breathe a %s of molten rock.", you.get_mutation_level(MUT_BREATHE_MAGMA) > 1 ? "torrent" : "surge");
            break;

        default:
        case ABIL_BREATHE_FIRE:
            zap = ZAP_BREATHE_FIRE;
            beam.origin_spell = SPELL_SEARING_BREATH;
            m   = "You breathe a blast of fire.";
            break;

        case ABIL_BREATHE_DART:
            zap = ZAP_BREATHE_DART;
            m   = "You spit a rudimentary magic dart.";
            break;

        case ABIL_BREATHE_CHAOS:
            zap = ZAP_BREATHE_CHAOS;
            m   = "You breathe a bolt of seething chaos.";
            beam.origin_spell = SPELL_BREATHE_CHAOTIC;
            break;

        case ABIL_BREATHE_FROST:
            zap = ZAP_BREATHE_FROST;
            beam.origin_spell = SPELL_CHILLING_BREATH;
            m   = "You exhale a wave of freezing cold.";
            break;

        case ABIL_BREATHE_POWER:
            zap = ZAP_BREATHE_POWER;
            m   = "You breathe a bolt of dispelling energy.";
            break;

        case ABIL_BREATHE_STEAM:
            zap = ZAP_BREATHE_STEAM;
            m   = "You exhale a blast of scalding steam.";
            break;

        case ABIL_BREATHE_RADIATION:
            zap = ZAP_BREATHE_RADIATION;
            m   = "You exhale a fountain of uncontrolled magic.";
            beam.hit_verb = "blasts";
            break;

        case ABIL_BREATHE_HOLY_FLAMES:
            zap = ZAP_BREATHE_HOLY_FLAMES;
            beam.origin_spell = SPELL_HOLY_BREATH;
            m   = "You exhale a cleansing burst of sacred fire.";
            break;

        case ABIL_BREATHE_MIASMA:
            zap = ZAP_BREATHE_MIASMA;
            m   = "You exhale a noxious wave of foul miasma.";
            if (empowered)
            {
                zap = ZAP_BREATHE_ROT;
                m = "You exhale a vile wave of vicious blight.";
            }
            break;

        case ABIL_BREATHE_DRAIN:
            zap = ZAP_BREATHE_DRAIN;
            m   = "You exhale a bolt of negative energy.";
            break;

        case ABIL_BREATHE_BUTTERFLIES:
            zap = ZAP_BREATHE_BUTTERFLY;
            m   = "You exhale a cloud of butterflies.";
            beam.hit_verb = "sparkles around";
            break;
        }

        if (empowered && zap != ZAP_BREATHE_CHAOS)
            beam.origin_spell = SPELL_EMPOWERED_BREATH;

        if (zapping(zap, power, beam, true, m.c_str()) == spret::abort)
            return spret::abort;

        if (empowered && zap == ZAP_BREATHE_CHAOS)
            create_vortices(&you);

        if (zap == ZAP_BREATHE_BUTTERFLY)
        {
            int extras = 2 + you.get_experience_level() / 9 + random2(4);
            for (int i = 0; i < extras; i++)
            {
                monster_type butttype = empowered && x_chance_in_y(power, 120) ? MONS_SPHINX_MOTH : MONS_BUTTERFLY;

                monster * butterfly = create_monster(mgen_data(butttype, BEH_COPY, you.pos(),
                                                     MHITYOU).set_summoned(&you, 2, SPELL_NO_SPELL, GOD_NO_GOD));
                
                if (butterfly)
                {
                    for (adjacent_iterator ai(you.pos()); ai; ++ai)
                    {
                        if (!actor_at(*ai) && butterfly->is_habitable(*ai))
                        {
                            butterfly->move_to_pos(*ai);
                            break;
                        }
                    }

                    if (butttype == MONS_SPHINX_MOTH)
                    {
                        butterfly->set_hit_dice(3 + div_rand_round(power, 5));
                        butterfly->max_hit_points = butterfly->hit_points = butterfly->max_hit_points * butterfly->get_hit_dice() / 10;
                    }

                    mon_enchant abj = butterfly->get_ench(ENCH_ABJ);

                    // Matches the damage roll of the beam.
                    abj.duration = (roll_dice(5, 4 + you.get_experience_level() / 3) * BASELINE_DELAY);
                    butterfly->update_ench(abj);
                }
            }
        }

        mount_drake_breath(&beam);

        you.increase_duration(DUR_BREATH_WEAPON,
                      3 + random2(10) + random2(30 - you.experience_level));
        break;
    }

    case ABIL_EVOKE_BLINK:      // randarts
        fail_check();
        // deliberate fall-through
    case ABIL_BLINK:            // mutation
        return cast_blink(fail);

    case ABIL_EVOKE_BERSERK:    // amulet of rage, randarts
        fail_check();
        you.go_berserk(true);
        break;

    case ABIL_FLY:
        fail_check();
        // Te or Dr/Gr wings
        if (you.racial_permanent_flight())
        {
            you.attribute[ATTR_PERM_FLIGHT] = 1;
            float_player();
        }
        if (you.get_mutation_level(MUT_TENGU_FLIGHT))
            mpr("You feel very comfortable in the air.");
        break;

    case ABIL_PLANT_ROOTS:
        fail_check();
        mpr("Your roots penetrate the ground.");
        mpr("You feel a comforting sense of stasis.");

        if (you.petrifying() || you.petrified())
            mpr("Your stasis cancels the petrification.");

        // BCADNOTE: If more sources of gaining stasis are added, add this to them too.
        you.duration[DUR_PETRIFYING] = 0;
        you.duration[DUR_PETRIFIED] = 0;

        you.attribute[ATTR_ROOTED] = 1;
        break;

    case ABIL_DEROOT:
        fail_check();
        start_delay<DerootDelay>(8);
        break;

    // DEMONIC POWERS:
    case ABIL_HELLFIRE:
        fail_check();
        if (your_spells(SPELL_HURL_HELLFIRE,
                        you.experience_level * 10,
                        false) == spret::abort)
        {
            return spret::abort;
        }
        break;

    // Jiyva powers:
    case ABIL_TURN_INVISIBLE:
        if (!invis_allowed())
            return spret::abort;
        fail_check();
        you.props[INVIS_CONTAMLESS_KEY].get_bool() = true;
        potionlike_effect(POT_INVISIBILITY, 20 + you.experience_level + you.skill(SK_INVOCATIONS));
        return spret::success;

    case ABIL_BUD_EYEBALLS:
        fail_check();
        _spawn_eyeballs();
        return spret::success;

    case ABIL_SILENT_SCREAM:
        return cast_silence((6 + you.experience_level + you.skill(SK_INVOCATIONS)) * 2, fail, true);

    case ABIL_FROST_BURST:
        return cast_starburst(6 + you.experience_level + you.skill(SK_INVOCATIONS), fail, false, true);

    case ABIL_CORROSIVE_WAVE:
    {
        fail_check();
        const int pow = 6 + you.experience_level + you.skill(SK_INVOCATIONS);
        zappy(ZAP_CORROSIVE_WAVE, pow, false, beam);
        beam.range = 4;
        beam.origin_spell = SPELL_PRIMAL_WAVE;

        direction_chooser_args args;
        args.mode = TARG_HOSTILE;
        args.top_prompt = "Squirt your ooze at?";
        args.self = confirm_prompt_type::cancel;

        if (!spell_direction(abild, beam, &args) || !player_tracer(ZAP_CORROSIVE_WAVE, pow, beam))
            return spret::abort;

        beam.fire();
        break;
    }

    case ABIL_SLIME_BOLT:
        return blink_bolt(fail, 6 + you.experience_level + you.skill(SK_INVOCATIONS));

    case ABIL_SUBSUME:
        if (subsume_item())
            return spret::success;
        return spret::abort;

    case ABIL_EJECT:
        if (eject_item())
            return spret::success;
        return spret::abort;

    case ABIL_EVOKE_TURN_INVISIBLE:     // cloaks, randarts
        if (!invis_allowed())
            return spret::abort;
        fail_check();
#if TAG_MAJOR_VERSION == 34
        surge_power(you.spec_evoke());
#endif
        potionlike_effect(POT_INVISIBILITY,
                          player_adjust_evoc_power(
                              you.skill(SK_EVOCATIONS, 2) + 5));
        contaminate_player(1000 + random2(2000), true);
        break;

#if TAG_MAJOR_VERSION == 34
    case ABIL_EVOKE_TURN_VISIBLE:
        fail_check();
        ASSERT(!you.attribute[ATTR_INVIS_UNCANCELLABLE]);
        mpr("You feel less transparent.");
        you.duration[DUR_INVIS] = 1;
        break;
#endif

    case ABIL_EVOKE_FLIGHT:             // randarts
        fail_check();
        ASSERT(!get_form()->forbids_flight());
#if TAG_MAJOR_VERSION == 34
        surge_power(you.spec_evoke());
#endif
        fly_player(player_adjust_evoc_power(you.skill(SK_EVOCATIONS, 2) + 30));
        break;

    case ABIL_EVOKE_FOG:     // cloak of the Thief
        fail_check();
        mpr("With a swish of your cloak, you release a cloud of fog.");
        big_cloud(random_smoke_type(), &you, you.pos(), 50, 8 + random2(8));
        break;

    case ABIL_EVOKE_RATSKIN: // ratskin cloak
        fail_check();
        mpr("The rats of the Dungeon answer your call.");

        for (int i = 0; i < (coinflip() + 1); ++i)
        {
            monster_type mon = coinflip() ? MONS_HELL_RAT : MONS_SEWER_RAT;

            mgen_data mg(mon, BEH_FRIENDLY, you.pos(), MHITYOU);
            if (monster *m = create_monster(mg))
                m->add_ench(mon_enchant(ENCH_FAKE_ABJURATION, 3));
        }

        break;

    case ABIL_EVOKE_THUNDER: // robe of Clouds
        fail_check();
        mpr("The folds of your robe billow into a mighty storm.");

        for (radius_iterator ri(you.pos(), 2, C_SQUARE); ri; ++ri)
            if (!cell_is_solid(*ri))
                place_cloud(CLOUD_STORM, *ri, 8 + random2avg(8,2), &you);

        break;

    case ABIL_CANCEL_PPROJ:
        fail_check();
        you.duration[DUR_PORTAL_PROJECTILE] = 0;
        you.attribute[ATTR_PORTAL_PROJECTILE] = 0;
        mpr("You are no longer teleporting projectiles to their destination.");
        break;

    case ABIL_STOP_FLYING:
        fail_check();
        you.duration[DUR_FLIGHT] = 0;
        you.attribute[ATTR_PERM_FLIGHT] = 0;
        land_player();
        break;

    case ABIL_END_TRANSFORMATION:
        fail_check();
        untransform();
        break;

    case ABIL_END_UPRISING:
        fail_check();
        mprf(MSGCH_DURATION, "You stop raising skeletons from your steps.");
        you.attribute[ATTR_SKELETON] = 0;
        break;

    case ABIL_DISMOUNT:
        fail_check();
        mprf(MSGCH_DURATION, "You dismiss your mount.");
        dismount();
        break;

    // INVOCATIONS:
    case ABIL_ZIN_RECITE:
    {
        fail_check();
        if (zin_check_recite_to_monsters() == 1)
        {
            you.attribute[ATTR_RECITE_TYPE] = (recite_type) random2(NUM_RECITE_TYPES); // This is just flavor
            you.attribute[ATTR_RECITE_SEED] = random2(2187); // 3^7
            you.duration[DUR_RECITE] = 3 * BASELINE_DELAY;
            mprf("You clear your throat and prepare to recite.");
            you.increase_duration(DUR_RECITE_COOLDOWN,
                                  3 + random2(10) + random2(30));
        }
        else
        {
            canned_msg(MSG_OK);
            return spret::abort;
        }
        break;
    }
    case ABIL_ZIN_VITALISATION:
        fail_check();
        zin_vitalisation();
        break;

    case ABIL_ZIN_IMPRISON:
    {
        beam.range = LOS_MAX_RANGE;
        direction_chooser_args args;
        args.restricts = DIR_TARGET;
        args.mode = TARG_HOSTILE;
        args.needs_path = false;
        if (!spell_direction(spd, beam, &args))
            return spret::abort;

        if (beam.target == you.pos())
        {
            mpr("You cannot imprison yourself!");
            return spret::abort;
        }

        monster* mons = monster_at(beam.target);

        if (mons == nullptr || !(you.can_see(*mons)))
        {
            mpr("There is no monster there to imprison!");
            return spret::abort;
        }

        if (mons_is_firewood(*mons) || mons_is_conjured(mons->type))
        {
            mpr("You cannot imprison that!");
            return spret::abort;
        }

        if (mons->friendly() || mons->good_neutral())
        {
            mpr("You cannot imprison a law-abiding creature!");
            return spret::abort;
        }

        fail_check();

        int power = apply_invo_enhancer(3 + (roll_dice(5, you.skill(SK_INVOCATIONS, 5) + 12) / 26),true);

        if (!cast_imprison(power, mons, -GOD_ZIN))
            return spret::abort;
        break;
    }

    case ABIL_ZIN_SANCTUARY:
        fail_check();
        zin_sanctuary();
        break;

    case ABIL_ZIN_DONATE_GOLD:
        fail_check();
        zin_donate_gold();
        break;

    case ABIL_TSO_DIVINE_SHIELD:
        fail_check();
        tso_divine_shield();
        break;

    case ABIL_TSO_CLEANSING_FLAME:
    {
        targeter_radius hitfunc(&you, LOS_SOLID, 2);
        {
            if (stop_attack_prompt(hitfunc, "harm", _cleansing_flame_affects))
                return spret::abort;
        }
        fail_check();
        cleansing_flame(apply_invo_enhancer(10 + you.skill_rdiv(SK_INVOCATIONS, 7, 6),true),
                        cleansing_flame_source::invocation, you.pos(), &you);
        break;
    }

    case ABIL_TSO_SUMMON_DIVINE_WARRIOR:
        fail_check();
        summon_holy_warrior(apply_invo_enhancer(you.skill(SK_INVOCATIONS, 4),true), false);
        break;

    case ABIL_TSO_BLESS_WEAPON:
        fail_check();
        simple_god_message(" will bless one of your weapons.");
        // included in default force_more_message
        if (!bless_weapon(GOD_SHINING_ONE, SPWPN_HOLY_WRATH, YELLOW))
            return spret::abort;
        break;

    case ABIL_KIKU_OPEN_CRYPTS:
        you.attribute[ATTR_KIKU_CORPSE] = 1;
        break;

    case ABIL_KIKU_CLOSE_CRYPTS:
        you.attribute[ATTR_KIKU_CORPSE] = 0;
        break;

    case ABIL_KIKU_BLESS_WEAPON:
        fail_check();
        simple_god_message(" will bloody one of your weapons with pain.");
        // included in default force_more_message
        if (!bless_weapon(GOD_KIKUBAAQUDGHA, SPWPN_PAIN, RED))
            return spret::abort;
        break;

    case ABIL_KIKU_GIFT_NECRONOMICON:
    {
        fail_check();
        if (!final_book_gift(GOD_KIKUBAAQUDGHA))
            return spret::abort;
        break;
    }

    case ABIL_YRED_INJURY_MIRROR:
        fail_check();

        you.duration[DUR_MIRROR_DAMAGE] += (apply_invo_enhancer(3
            + random2avg(you.skill(SK_INVOCATIONS, 2), 2), true) * BASELINE_DELAY);

        if (yred_injury_mirror())
            mpr("Another wave of unholy energy enters you.");
        else
        {
            mprf("You offer yourself to %s, and are filled with unholy energy.",
                 god_name(you.religion).c_str());
        }

        break;

    case ABIL_YRED_ANIMATE_REMAINS:
        fail_check();
        canned_msg(MSG_CALL_DEAD);
        twisted_resurrection(&you, 200, BEH_FRIENDLY, MHITYOU, GOD_YREDELEMNUL);
        break;

    case ABIL_YRED_RECALL_UNDEAD_SLAVES:
        fail_check();
        start_recall(recall_t::yred);
        break;

    case ABIL_YRED_DRAIN_LIFE:
    {
        int damage = 0;
        const spret result =
            fire_los_attack_spell(SPELL_DRAIN_LIFE, apply_invo_enhancer(
                                  you.skill_rdiv(SK_INVOCATIONS), true),
                                  &you, nullptr, fail, &damage);
        if (result != spret::success)
            return result;

        if (damage > 0)
        {
            mpr("You feel life flooding into your body.");
            inc_hp(damage);
        }
        break;
    }

    case ABIL_YRED_ENSLAVE_SOUL:
    {
        god_acting gdact;
        beam.range = LOS_MAX_RANGE;
        direction_chooser_args args;
        args.restricts = DIR_TARGET;
        args.mode = TARG_HOSTILE;
        args.needs_path = false;

        if (!spell_direction(spd, beam, &args))
            return spret::abort;

        if (beam.target == you.pos())
        {
            mpr("Your soul already belongs to Yredelemnul.");
            return spret::abort;
        }

        monster* mons = monster_at(beam.target);
        if (mons == nullptr || !you.can_see(*mons)
            || !yred_can_enslave_soul(mons))
        {
            mpr("You see nothing there you can enslave the soul of!");
            return spret::abort;
        }

        // The monster can be no more than lightly wounded/damaged.
        if (mons_get_damage_level(*mons) > MDAM_LIGHTLY_DAMAGED)
        {
            simple_monster_message(*mons, "'s soul is too badly injured.");
            return spret::abort;
        }
        fail_check();

        const int duration = apply_invo_enhancer(you.skill_rdiv(SK_INVOCATIONS, 3, 4) + 2,true);
        mons->add_ench(mon_enchant(ENCH_SOUL_RIPE, 0, &you,
                                   duration * BASELINE_DELAY));
        simple_monster_message(*mons, "'s soul is now ripe for the taking.");
        break;
    }

    case ABIL_OKAWARU_HEROISM:
        fail_check();
        previously_on = false;
        if (you.duration[DUR_HEROISM])
            previously_on = true;

        you.increase_duration(DUR_HEROISM,
                              apply_invo_enhancer(10 + random2avg(you.skill(SK_INVOCATIONS, 6), 2),true),
                              100);

        mprf(MSGCH_DURATION, previously_on
            ? "You feel more confident with your borrowed prowess."
            : "You gain the combat prowess of a mighty hero.");

        you.redraw_evasion      = true;
        you.redraw_armour_class = true;
        break;

    case ABIL_OKAWARU_FINESSE:
        fail_check();
        previously_on = false;
        if (you.duration[DUR_FINESSE])
            previously_on = true;
        
        you.increase_duration(DUR_FINESSE,
                              apply_invo_enhancer(10 + random2avg(you.skill(SK_INVOCATIONS, 6), 2),true),
                              100);

        if (previously_on)
        {
            // "Your [hand(s)] get{s} new energy."
            mprf(MSGCH_DURATION, "%s",
                you.hands_act("get", "new energy.").c_str());
        }
        else
            mprf(MSGCH_DURATION, "You can now deal lightning-fast blows.");

        did_god_conduct(DID_HASTY, 8); // Currently irrelevant.
        break;

    case ABIL_MAKHLEB_MINOR_DESTRUCTION:
    {
        beam.range = min((int)you.current_vision, 5);

        if (!spell_direction(spd, beam))
            return spret::abort;

        int power = apply_invo_enhancer(you.skill(SK_INVOCATIONS, 1)
                    + random2(1 + you.skill(SK_INVOCATIONS, 1))
                    + random2(1 + you.skill(SK_INVOCATIONS, 1)),true);

        // Since the actual beam is random, check with BEAM_MMISSILE.
        if (!player_tracer(ZAP_DEBUGGING_RAY, power, beam, beam.range))
            return spret::abort;

        fail_check();
        beam.origin_spell = SPELL_NO_SPELL; // let zapping reset this

        switch (random2(5))
        {
        case 0: zapping(ZAP_THROW_FLAME, power, beam); break;
        case 1: zapping(ZAP_PAIN, power, beam); break;
        case 2: zapping(ZAP_STONE_ARROW, power, beam); break;
        case 3: zapping(ZAP_SHOCK, power, beam); break;
        case 4: zapping(ZAP_BREATHE_ACID, power / 2, beam); break;
        }
        break;
    }

    case ABIL_MAKHLEB_LESSER_SERVANT_OF_MAKHLEB:
        summon_demon_type(random_choose(MONS_VLERK, MONS_NEQOXEC,
                                        MONS_NYCHDOD, MONS_MYGDARTH,
                                        MONS_YNOXINUL),
                          apply_invo_enhancer(20 + you.skill(SK_INVOCATIONS, 3),true),
                          GOD_MAKHLEB, 0, !fail);
        break;

    case ABIL_MAKHLEB_MAJOR_DESTRUCTION:
    {
        beam.range = you.current_vision;

        if (!spell_direction(spd, beam))
            return spret::abort;

        int power = apply_invo_enhancer(you.skill(SK_INVOCATIONS, 1)
                    + random2(1 + you.skill(SK_INVOCATIONS, 1))
                    + random2(1 + you.skill(SK_INVOCATIONS, 1)),true);

        // Since the actual beam is random, check with BEAM_MMISSILE.
        if (!player_tracer(ZAP_DEBUGGING_RAY, power, beam, beam.range))
            return spret::abort;

        fail_check();
        {
            beam.origin_spell = SPELL_NO_SPELL; // let zapping reset this
            zap_type ztype =
                random_choose(ZAP_BOLT_OF_FIRE,
                              ZAP_FIREBALL,
                              ZAP_LIGHTNING_BOLT,
                              ZAP_STICKY_FLAME,
                              ZAP_IRON_SHOT,
                              ZAP_BOLT_OF_DRAINING,
                              ZAP_ORB_OF_ELECTRICITY);
            zapping(ztype, power, beam);
        }
        break;
    }

    case ABIL_MAKHLEB_GREATER_SERVANT_OF_MAKHLEB:
        summon_demon_type(random_choose(MONS_OXARMORDTH, MONS_GWYRDD,
                                        MONS_CENYSUS, MONS_BALRUG,
                                        MONS_TITIVILUS),
                          apply_invo_enhancer(20 + you.skill(SK_INVOCATIONS, 3),true),
                          GOD_MAKHLEB, 0, !fail);
        break;

    case ABIL_TROG_BERSERK:
        fail_check();
        // Trog abilities don't use or train invocations.
        you.go_berserk(true);
        break;

    case ABIL_TROG_REGEN_MR:
        fail_check();
        // Trog abilities don't use or train invocations.
        trog_do_trogs_hand();
        break;

    case ABIL_TROG_BROTHERS_IN_ARMS:
        fail_check();
        // Trog abilities don't use or train invocations.
        you.berserk_penalty = 0;
        you.increase_duration(DUR_BERSERK, 3);
        summon_berserker(you.piety +
                         random2(you.piety/4) - random2(you.piety/4),
                         &you);
        break;

    case ABIL_SIF_MUNA_DIVINE_ENERGY:
        simple_god_message(" will now grant you divine energy when your "
                           "reserves of magic are depleted.");
        mpr("You will briefly lose access to your magic after casting a "
            "spell in this manner.");
        you.attribute[ATTR_DIVINE_ENERGY] = 1;
        break;

    case ABIL_SIF_MUNA_STOP_DIVINE_ENERGY:
        simple_god_message(" stops granting you divine energy.");
        you.attribute[ATTR_DIVINE_ENERGY] = 0;
        break;

    case ABIL_SIF_MUNA_FORGET_SPELL:
        fail_check();
        if (cast_selective_amnesia() <= 0)
        {
            canned_msg(MSG_OK);
            return spret::abort;
        }
        break;

    case ABIL_SIF_MUNA_CHANNEL_ENERGY:
    {
        fail_check();
        you.increase_duration(DUR_CHANNEL_ENERGY,
            apply_invo_enhancer(4 + random2avg(you.skill_rdiv(SK_INVOCATIONS, 2, 3), 2), true));
        break;
    }

    case ABIL_ELYVILON_LIFESAVING:
        fail_check();
        if (you.duration[DUR_LIFESAVING])
            mpr("You renew your call for help.");
        else
        {
            mprf("You beseech %s to protect your life.",
                 god_name(you.religion).c_str());
        }
        // Might be a decrease, this is intentional (like Yred).
        you.duration[DUR_LIFESAVING] = apply_pity(9 * BASELINE_DELAY
                     + random2avg(you.piety * BASELINE_DELAY, 2) / 10);
        break;

    case ABIL_ELYVILON_LESSER_HEALING:
    case ABIL_ELYVILON_GREATER_HEALING:
    {
        fail_check();
        int pow = 0;
        if (abil.ability == ABIL_ELYVILON_LESSER_HEALING)
            pow = apply_invo_enhancer(3 + you.skill_rdiv(SK_INVOCATIONS, 1, 6),true);
        else
            pow = apply_invo_enhancer(10 + you.skill_rdiv(SK_INVOCATIONS, 1, 3), true);
        pow = min(50, pow);
        if (you.species == SP_FAIRY)
            pow = div_rand_round(pow, 10);
        pow = max(1, pow);
        const int healed = pow + roll_dice(2, pow) - 2;
        mpr("You are healed.");
        inc_hp(healed);
        break;
    }

    case ABIL_ELYVILON_PURIFICATION:
        fail_check();
        elyvilon_purification();
        break;

    case ABIL_ELYVILON_HEAL_OTHER:
    {
        int pow = 30 + you.skill(SK_INVOCATIONS, 1);
        return cast_healing(pow, fail);
    }

    case ABIL_ELYVILON_DIVINE_VIGOUR:
        fail_check();
        if (!elyvilon_divine_vigour())
            return spret::abort;
        break;

    case ABIL_LUGONU_ABYSS_EXIT:
        fail_check();
        down_stairs(DNGN_EXIT_ABYSS);
        break;

    case ABIL_LUGONU_BEND_SPACE:
        fail_check();
        if (!lugonu_bend_space())
            return spret::abort;
        break;

    case ABIL_LUGONU_BANISH:
    {
        beam.range = you.current_vision;
        const int pow = 68 + you.skill(SK_INVOCATIONS, 3);

        direction_chooser_args args;
        args.mode = TARG_HOSTILE;
        args.get_desc_func = bind(desc_success_chance, placeholders::_1,
                                  zap_ench_power(ZAP_BANISHMENT, pow, false),
                                  false, nullptr);
        if (!spell_direction(spd, beam, &args))
            return spret::abort;

        if (beam.target == you.pos())
        {
            mpr("You cannot banish yourself!");
            return spret::abort;
        }

        fail_check();

        return zapping(ZAP_BANISHMENT, apply_invo_enhancer(pow,true), beam, true, nullptr, fail);
    }

    case ABIL_LUGONU_CORRUPT:
        fail_check();
        if (!lugonu_corrupt_level(apply_invo_enhancer(300 + you.skill(SK_INVOCATIONS, 15),true)))
            return spret::abort;
        break;

    case ABIL_LUGONU_ABYSS_ENTER:
    {
        fail_check();
        // Deflate HP.
        dec_hp(random2avg(you.hp, 2), false);

        no_notes nx; // This banishment shouldn't be noted.
        banished();
        break;
    }

    case ABIL_LUGONU_BLESS_WEAPON:
        fail_check();
        simple_god_message(" will brand one of your weapons with the "
                           "corruption of the Abyss.");
        // included in default force_more_message
        if (!bless_weapon(GOD_LUGONU, SPWPN_DISTORTION, MAGENTA))
            return spret::abort;
        break;

    case ABIL_NEMELEX_TRIPLE_DRAW:
        fail_check();
        if (!deck_triple_draw())
            return spret::abort;
        break;

    case ABIL_NEMELEX_DEAL_FOUR:
        fail_check();
        if (!deck_deal())
            return spret::abort;
        break;

    case ABIL_NEMELEX_STACK_FIVE:
        fail_check();
        if (!deck_stack())
            return spret::abort;
        break;

    case ABIL_BEOGH_SMITING:
        fail_check();
        if (your_spells(SPELL_SMITING,
                        apply_invo_enhancer(12 + skill_bump(SK_INVOCATIONS, 6),true),
                        false, nullptr) == spret::abort)
        {
            return spret::abort;
        }
        break;

    case ABIL_BEOGH_GIFT_ITEM:
        if (!beogh_gift_item())
            return spret::abort;
        break;

    case ABIL_BEOGH_RESURRECTION:
        if (!beogh_resurrect())
            return spret::abort;
        break;

    case ABIL_BEOGH_RECALL_ORCISH_FOLLOWERS:
        fail_check();
        start_recall(recall_t::beogh);
        break;

    case ABIL_STOP_RECALL:
        fail_check();
        mpr("You stop recalling your allies.");
        end_recall();
        break;

    case ABIL_FEDHAS_FUNGAL_BLOOM:
        fedhas_fungal_bloom();
        return spret::success;

    case ABIL_FEDHAS_SUNLIGHT:
        return fedhas_sunlight(fail);

    case ABIL_FEDHAS_PLANT_RING:
        fail_check();
        if (!fedhas_plant_ring())
            return spret::abort;
        break;

    case ABIL_FEDHAS_RAIN:
        fail_check();
        if (!fedhas_rain(you.pos()))
        {
            canned_msg(MSG_NOTHING_HAPPENS);
            return spret::abort;
        }
        break;

    case ABIL_FEDHAS_SPAWN_SPORES:
    {
        fail_check();
        const int num = fedhas_corpse_spores();
        ASSERT(num > 0);
        break;
    }

    case ABIL_FEDHAS_EVOLUTION:
        return fedhas_evolve_flora(fail);

    case ABIL_TRAN_BAT:
        fail_check();
        if (!transform(100, transformation::bat))
        {
            crawl_state.zero_turns_taken();
            return spret::abort;
        }
        break;

    case ABIL_JIYVA_DISSOLUTION:
    {
        fail_check();
        jiyva_dissolution();
        break;
    }

    case ABIL_JIYVA_SET_TARGETS_FREE:
    case ABIL_JIYVA_SET_TARGETS:
    {
        jiyva_set_targets();
        break;
    }

    case ABIL_JIYVA_SLIMIFY:
    {
        fail_check();

        if (you.weapon(0) && ((you.weapon(0)->base_type == OBJ_SHIELDS && !is_hybrid(you.weapon(0)->sub_type)) || is_range_weapon(*you.weapon(0))))
        { 
            if (you.hands_reqd(*you.weapon(0)) == HANDS_TWO)
            {
                mpr("You need a free hand or a melee weapon in one hand to use this ability.");
                return spret::abort;
            }
            else if (you.weapon(1) && ((you.weapon(1)->base_type == OBJ_SHIELDS && !is_hybrid(you.weapon(1)->sub_type)) || is_range_weapon(*you.weapon(1))))
            {
                mpr("You need a free hand or a melee weapon in one hand to use this ability.");
                return spret::abort;
            }
        }

        const int slime = _slime_count(true);

        string msg = "";
        if (you.weapon(0) && is_melee_weapon(*you.weapon(0)))
            msg = you.weapon(0)->name(DESC_YOUR);
        else if (you.weapon(1) && is_melee_weapon(*you.weapon(1)))
            msg = you.weapon(1)->name(DESC_YOUR);
        else
            msg = "your " + you.hand_name(true);
        mprf(MSGCH_DURATION, "The slime in your surroundings rushes to %s.", msg.c_str());
        you.increase_duration(DUR_SLIMIFY,
                              apply_invo_enhancer(random2avg(you.piety / 4, 2) + random2(slime), false), 100);
        break;
    }

    case ABIL_JIYVA_SLIME_MOUNT:
        return gain_mount(mount_type::slime, 0, fail);

    case ABIL_CHEIBRIADOS_TIME_STEP:
        fail_check();
        cheibriados_time_step(max(1, you.skill(SK_INVOCATIONS, 10)
                                     * you.piety / 100));
        break;

    case ABIL_CHEIBRIADOS_TIME_BEND:
        fail_check();
        cheibriados_time_bend(16 + you.skill(SK_INVOCATIONS, 8));
        break;

    case ABIL_CHEIBRIADOS_DISTORTION:
        fail_check();
        cheibriados_temporal_distortion();
        break;

    case ABIL_CHEIBRIADOS_SLOUCH:
        fail_check();
        if (!cheibriados_slouch())
            return spret::abort;
        break;

    case ABIL_ASHENZARI_CURSE:
    {
        fail_check();
        if (!ashenzari_curse_item())
            return spret::abort;
        break;
    }

    case ABIL_ASHENZARI_SCRYING:
        fail_check();
        you.attribute[ATTR_SCRYING] = !you.attribute[ATTR_SCRYING];
        if (you.attribute[ATTR_SCRYING])
        {
            mpr("You peer through the fabric of reality.");
            you.xray_vision = true;
        }
        else
        {
            mpr("You limit yourself to natural vision.");
            you.xray_vision = false;
        }
        viewwindow(true);
        break;

    case ABIL_ASHENZARI_TRANSFER_KNOWLEDGE:
        fail_check();
        if (!ashenzari_transfer_knowledge())
        {
            canned_msg(MSG_OK);
            return spret::abort;
        }
        break;

    case ABIL_ASHENZARI_END_TRANSFER:
        fail_check();
        if (!ashenzari_end_transfer())
        {
            canned_msg(MSG_OK);
            return spret::abort;
        }
        break;

    case ABIL_DITHMENOS_SHADOW_STEP:
        if (_abort_if_stationary())
            return spret::abort;
        fail_check();
        if (!dithmenos_shadow_step())
        {
            canned_msg(MSG_OK);
            return spret::abort;
        }
        break;

    case ABIL_DITHMENOS_SHADOW_FORM:
        fail_check();
        if (!transform(apply_invo_enhancer(you.skill(SK_INVOCATIONS, 2),true), transformation::shadow))
        {
            crawl_state.zero_turns_taken();
            return spret::abort;
        }
        break;

    case ABIL_GOZAG_POTION_PETITION:
        fail_check();
        run_uncancel(UNC_POTION_PETITION, 0);
        break;

    case ABIL_GOZAG_CALL_MERCHANT:
        fail_check();
        run_uncancel(UNC_CALL_MERCHANT, 0);
        break;

    case ABIL_GOZAG_BRIBE_BRANCH:
        fail_check();
        if (!gozag_bribe_branch())
            return spret::abort;
        break;

    case ABIL_QAZLAL_UPHEAVAL:
        return qazlal_upheaval(coord_def(), false, fail);

    case ABIL_QAZLAL_ELEMENTAL_FORCE:
        return qazlal_elemental_force(fail);

    case ABIL_QAZLAL_DISASTER_AREA:
        fail_check();
        if (!qazlal_disaster_area())
            return spret::abort;
        break;

    case ABIL_RU_SACRIFICE_PURITY:
    case ABIL_RU_SACRIFICE_WORDS:
    case ABIL_RU_SACRIFICE_DRINK:
    case ABIL_RU_SACRIFICE_ESSENCE:
    case ABIL_RU_SACRIFICE_HEALTH:
    case ABIL_RU_SACRIFICE_STEALTH:
    case ABIL_RU_SACRIFICE_ARTIFICE:
    case ABIL_RU_SACRIFICE_LOVE:
    case ABIL_RU_SACRIFICE_COURAGE:
    case ABIL_RU_SACRIFICE_ARCANA:
    case ABIL_RU_SACRIFICE_NIMBLENESS:
    case ABIL_RU_SACRIFICE_DURABILITY:
    case ABIL_RU_SACRIFICE_HAND:
    case ABIL_RU_SACRIFICE_EXPERIENCE:
    case ABIL_RU_SACRIFICE_SKILL:
    case ABIL_RU_SACRIFICE_EYE:
    case ABIL_RU_SACRIFICE_RESISTANCE:
        fail_check();
        if (!ru_do_sacrifice(abil.ability))
            return spret::abort;
        break;

    case ABIL_RU_REJECT_SACRIFICES:
        fail_check();
        if (!ru_reject_sacrifices())
            return spret::abort;
        break;

    case ABIL_RU_DRAW_OUT_POWER:
        fail_check();
        if (you.duration[DUR_EXHAUSTED])
        {
            mpr("You're too exhausted to draw out your power.");
            return spret::abort;
        }
        if (you.hp == you.hp_max && you.magic_points == you.max_magic_points
            && !you.duration[DUR_CONF]
            && !you.duration[DUR_SLOW]
            && !you.attribute[ATTR_HELD]
            && !you.petrifying()
            && !you.is_constricted())
        {
            mpr("You have no need to draw out power.");
            return spret::abort;
        }
        ru_draw_out_power();
        you.increase_duration(DUR_EXHAUSTED, 12 + random2(5));
        break;

    case ABIL_RU_POWER_LEAP:
        if (you.duration[DUR_EXHAUSTED])
        {
            mpr("You're too exhausted to power leap.");
            return spret::abort;
        }

        if (_abort_if_stationary())
            return spret::abort;

        fail_check();

        if (!ru_power_leap())
        {
            canned_msg(MSG_OK);
            return spret::abort;
        }
        you.increase_duration(DUR_EXHAUSTED, 18 + random2(8));
        break;

    case ABIL_RU_APOCALYPSE:
        if (you.duration[DUR_EXHAUSTED])
        {
            mpr("You're too exhausted to unleash your apocalyptic power.");
            return spret::abort;
        }

        fail_check();

        if (!ru_apocalypse())
            return spret::abort;
        you.increase_duration(DUR_EXHAUSTED, 30 + random2(20));
        break;

#if TAG_MAJOR_VERSION == 34
    case ABIL_PAKELLAS_DEVICE_SURGE:
    {
        fail_check();

        mprf(MSGCH_DURATION, "You feel a buildup of energy.");
        you.increase_duration(DUR_DEVICE_SURGE,
                              random2avg(you.piety / 4, 2) + 3, 100);
        break;
    }
#endif

    case ABIL_USKAYAW_STOMP:
        fail_check();
        if (!uskayaw_stomp())
            return spret::abort;
        break;

    case ABIL_USKAYAW_LINE_PASS:
        if (_abort_if_stationary())
            return spret::abort;
        fail_check();
        if (!uskayaw_line_pass())
            return spret::abort;
        break;

    case ABIL_USKAYAW_GRAND_FINALE:
        return uskayaw_grand_finale(fail);

    case ABIL_HEPLIAKLQANA_IDEALISE:
        return hepliaklqana_idealise(fail);

    case ABIL_HEPLIAKLQANA_RECALL:
        fail_check();
        if (try_recall(hepliaklqana_ancestor()))
            upgrade_hepliaklqana_ancestor(true);
        break;

    case ABIL_HEPLIAKLQANA_TRANSFERENCE:
        return hepliaklqana_transference(fail);

    case ABIL_HEPLIAKLQANA_TYPE_KNIGHT:
    case ABIL_HEPLIAKLQANA_TYPE_BATTLEMAGE:
    case ABIL_HEPLIAKLQANA_TYPE_HEXER:
        if (!hepliaklqana_choose_ancestor_type(abil.ability))
            return spret::abort;
        break;

    case ABIL_HEPLIAKLQANA_IDENTITY:
        hepliaklqana_choose_identity();
        break;

    case ABIL_WU_JIAN_SERPENTS_LASH:
        if (you.attribute[ATTR_SERPENTS_LASH])
        {
            mpr("You are already lashing out.");
            return spret::abort;
        }
        if (you.duration[DUR_EXHAUSTED])
        {
            mpr("You are too exhausted to lash out.");
            return spret::abort;
        }
        fail_check();
        you.attribute[ATTR_SERPENTS_LASH] = apply_invo_enhancer(2, true);
        mprf(MSGCH_GOD, "Your muscles tense, ready for explosive movement...");
        you.redraw_status_lights = true;
        return spret::success;

    case ABIL_WU_JIAN_HEAVENLY_STORM:
        fail_check();

        you.attribute[ATTR_HEAVENLY_STORM] = apply_invo_enhancer(12, true);

        mprf(MSGCH_GOD, "The air is filled with shimmering golden clouds!");
        wu_jian_sifu_message(" says: The storm will not cease as long as you "
                             "keep fighting, disciple!");

        for (radius_iterator ai(you.pos(), 2, C_SQUARE, LOS_SOLID); ai; ++ai)
        {
            if (!cell_is_solid(*ai))
                place_cloud(CLOUD_GOLD_DUST, *ai, 5 + random2(5), &you);
        }

        you.duration[DUR_HEAVENLY_STORM] = WU_JIAN_HEAVEN_TICK_TIME;
        invalidate_agrid(true);
        break;

    case ABIL_WU_JIAN_WALLJUMP:
        fail_check();
        return wu_jian_wall_jump_ability();

    case ABIL_BAHAMUT_PROTECTION:
    case ABIL_TIAMAT_RETRIBUTION:
    case ABIL_CHOOSE_BAHAMUT_BREATH:
    case ABIL_CHOOSE_TIAMAT_BREATH:
    case ABIL_CHOOSE_BAHAMUT_DRAKE:
    case ABIL_CHOOSE_TIAMAT_DRAKE:
    case ABIL_BAHAMUT_TRANSFORM:
    case ABIL_CHOOSE_TIAMAT_TRANSFORM:
        if (!bahamut_tiamat_make_choice(abil.ability))
            return spret::abort;
        break;

    case ABIL_BAHAMUT_EMPOWERED_BREATH:
        fail_check();
        return bahamut_empowered_breath();

    case ABIL_TIAMAT_ADAPTIVE_BREATH:
        return tiamat_choice_breath(fail);

    case ABIL_BAHAMUT_DRAKE_MOUNT:
        return gain_mount(mount_type::drake, 0, fail);

    case ABIL_TIAMAT_SUMMON_DRAKES:
        fail_check();
        if (summon_drakes(apply_invo_enhancer(you.skill(SK_INVOCATIONS), true), false))
            return spret::success;
        return spret::fail;

    case ABIL_TIAMAT_TRANSFORM:
        fail_check();
        return (bahamut_tiamat_transform(false));

    case ABIL_BAHAMUT_DRAGONSLAYING:
        fail_check();
        mprf(MSGCH_GOD, "Bahamut will enhance one of your weapons.");
        // included in default force_more_message
        if (!bless_weapon(GOD_BAHAMUT_TIAMAT, SPWPN_DRAGON_SLAYING, YELLOW))
            return spret::abort;
        break;

    case ABIL_TIAMAT_DRAGON_BOOK:
        fail_check();
        if (!final_book_gift(GOD_BAHAMUT_TIAMAT))
            return spret::abort;
        break;

    case ABIL_RENOUNCE_RELIGION:
        fail_check();
        if (yesno("Really renounce your faith, foregoing its fabulous benefits?",
                  false, 'n')
            && yesno("Are you sure?", false, 'n'))
        {
            excommunication(true);
        }
        else
        {
            canned_msg(MSG_OK);
            return spret::abort;
        }
        break;

    case ABIL_CONVERT_TO_BEOGH:
        fail_check();
        god_pitch(GOD_BEOGH);
        if (you_worship(GOD_BEOGH))
        {
            spare_beogh_convert();
            break;
        }
        return spret::abort;

    case ABIL_NON_ABILITY:
        fail_check();
        mpr("Sorry, you can't do that.");
        break;

    default:
        die("invalid ability");
    }

    return spret::success;
}

static void _pay_ability_costs(const ability_def& abil)
{
    // wall jump handles its own timing, because it can be instant if
    // serpent's lash is activated.
    if (abil.flags & abflag::instant)
    {
        you.turn_is_over = false;
        you.elapsed_time_at_last_input = you.elapsed_time;
        update_turn_count();
    }
    else if (abil.ability != ABIL_WU_JIAN_WALLJUMP)
        you.turn_is_over = true;

    const int food_cost  = div_rand_round(abil.food_cost * (40 + random2(20)), 100);
    const int piety_cost = abil.piety_cost;
    const int hp_cost    = abil.hp_cost.cost(you.hp_max);

    dprf("Cost: mp=%d; hp=%d; food=%d; piety=%d",
        ability_mp_cost(abil.ability), hp_cost, food_cost, piety_cost);

    if (ability_mp_cost(abil.ability))
        dec_mp(ability_mp_cost(abil.ability));

    if (abil.hp_cost)
        dec_hp(hp_cost, false);

    if (food_cost)
        make_hungry(food_cost, false);

    if (piety_cost)
        lose_piety(piety_cost);
}

int choose_ability_menu(const vector<talent>& talents)
{
    ToggleableMenu abil_menu(MF_SINGLESELECT | MF_ANYPRINTABLE
            | MF_NO_WRAP_ROWS | MF_TOGGLE_ACTION | MF_ALWAYS_SHOW_MORE);

    abil_menu.set_highlighter(nullptr);
#ifdef USE_TILE_LOCAL
    {
        // Hack like the one in spl-cast.cc:list_spells() to align the title.
        ToggleableMenuEntry* me =
            new ToggleableMenuEntry("Ability - do what?                  "
                                    "Cost                            Failure",
                                    "Ability - describe what?            "
                                    "Cost                            Failure",
                                    MEL_ITEM);
        me->colour = BLUE;
        abil_menu.set_title(me, true, true);
    }
#else
    abil_menu.set_title(
        new ToggleableMenuEntry("Ability - do what?                  "
                                "Cost                            Failure",
                                "Ability - describe what?            "
                                "Cost                            Failure",
                                MEL_TITLE), true, true);
#endif
    abil_menu.set_tag("ability");
    abil_menu.add_toggle_key('!');
    abil_menu.add_toggle_key('?');
    abil_menu.menu_action = Menu::ACT_EXECUTE;

    if (crawl_state.game_is_hints())
    {
        // XXX: This could be buggy if you manage to pick up lots and
        // lots of abilities during hints mode.
        abil_menu.set_more(hints_abilities_info());
    }
    else
    {
        abil_menu.set_more(formatted_string::parse_string(
                           "Press '<w>!</w>' or '<w>?</w>' to toggle "
                           "between ability selection and description."));
    }

    int numbers[52];
    for (int i = 0; i < 52; ++i)
        numbers[i] = i;

    bool found_invocations = false;

    // First add all non-invocation abilities.
    for (unsigned int i = 0; i < talents.size(); ++i)
    {
        if (talents[i].is_invocation)
            found_invocations = true;
        else
        {
            ToggleableMenuEntry* me =
                new ToggleableMenuEntry(describe_talent(talents[i]),
                                        describe_talent(talents[i]),
                                        MEL_ITEM, 1, talents[i].hotkey);
            me->data = &numbers[i];
#ifdef USE_TILE
            me->add_tile(tile_def(tileidx_ability(talents[i].which), TEX_GUI));
#endif
            if (!check_ability_possible(talents[i].which, true))
            {
                me->colour = COL_INAPPLICABLE;
#ifdef USE_TILE
                me->add_tile(tile_def(TILEI_MESH, TEX_ICONS));
#endif
            }
            else if (_check_ability_dangerous(talents[i].which, true))
                me->colour = COL_DANGEROUS;
            // Only check this here, since your god can't hate its own abilities
            else if (god_hates_ability(talents[i].which, you.religion))
                me->colour = COL_FORBIDDEN;
            abil_menu.add_entry(me);
        }
    }

    if (found_invocations)
    {
#ifdef USE_TILE_LOCAL
        MenuEntry* subtitle = new MenuEntry(" Invocations -    ", MEL_ITEM);
        subtitle->colour = BLUE;
        abil_menu.add_entry(subtitle);
#else
        abil_menu.add_entry(new MenuEntry(" Invocations -    ", MEL_SUBTITLE));
#endif
        for (unsigned int i = 0; i < talents.size(); ++i)
        {
            if (talents[i].is_invocation)
            {
                ToggleableMenuEntry* me =
                    new ToggleableMenuEntry(describe_talent(talents[i]),
                                            describe_talent(talents[i]),
                                            MEL_ITEM, 1, talents[i].hotkey);
                me->data = &numbers[i];
#ifdef USE_TILE
                me->add_tile(tile_def(tileidx_ability(talents[i].which),
                                      TEX_GUI));
#endif
                if (!check_ability_possible(talents[i].which, true))
                {
                    me->colour = COL_INAPPLICABLE;
#ifdef USE_TILE
                    me->add_tile(tile_def(TILEI_MESH, TEX_ICONS));
#endif
                }
                else if (_check_ability_dangerous(talents[i].which, true))
                    me->colour = COL_DANGEROUS;
                abil_menu.add_entry(me);
            }
        }
    }

    int ret = -1;
    abil_menu.on_single_selection = [&abil_menu, &talents, &ret](const MenuEntry& sel)
    {
        ASSERT(sel.hotkeys.size() == 1);
        int selected = *(static_cast<int*>(sel.data));

        if (abil_menu.menu_action == Menu::ACT_EXAMINE)
            _print_talent_description(talents[selected]);
        else
            ret = *(static_cast<int*>(sel.data));
        return abil_menu.menu_action == Menu::ACT_EXAMINE;
    };
    abil_menu.show(false);
    if (!crawl_state.doing_prev_cmd_again)
        redraw_screen();
    return ret;
}

string describe_talent(const talent& tal)
{
    ASSERT(tal.which != ABIL_NON_ABILITY);

    const string failure = failure_rate_to_string(tal.fail)
        + (testbits(get_ability_def(tal.which).flags, abflag::hostile)
           ? " hostile" : "");

    ostringstream desc;
    desc << left
         << chop_string(ability_name(tal.which), 32)
         << chop_string(make_cost_description(tal.which), 32)
         << chop_string(failure, 12);
    return trimmed_string(desc.str());
}

static void _add_talent(vector<talent>& vec, const ability_type ability,
                        bool check_confused)
{
    const talent t = get_talent(ability, check_confused);
    if (t.which != ABIL_NON_ABILITY)
        vec.push_back(t);
}

/**
 * Return all relevant talents that the player has.
 *
 * Currently the only abilities that are affected by include_unusable are god
 * abilities (affect by e.g. penance or silence).
 * @param check_confused If true, abilities that don't work when confused will
 *                       be excluded.
 * @param include_unusable If true, abilities that are currently unusable will
 *                         be excluded.
 * @return  A vector of talent structs.
 */
vector<talent> your_talents(bool check_confused, bool include_unusable)
{
    vector<talent> talents;

    // Species-based abilities.
    if (you.species == SP_DEEP_DWARF)
        _add_talent(talents, ABIL_HEAL_WOUNDS, check_confused);

    if (you.get_mutation_level(MUT_BURROWING, false) && (form_keeps_mutations() || include_unusable))
    {
        _add_talent(talents, ABIL_DIG, check_confused);
        if (!crawl_state.game_is_sprint() || brdepth[you.where_are_you] > 1)
            _add_talent(talents, ABIL_SHAFT_SELF, check_confused);
    }

    if (you.get_mutation_level(MUT_FROG_LEGS, false) && (form_keeps_mutations() && !you.mounted() || include_unusable))
        _add_talent(talents, ABIL_HOP, check_confused);

    // Spit Poison, possibly upgraded to Breathe Poison.
    if (you.get_mutation_level(MUT_SPIT_POISON) == 2)
        _add_talent(talents, ABIL_BREATHE_POISON, check_confused);
    else if (you.get_mutation_level(MUT_SPIT_POISON))
        _add_talent(talents, ABIL_SPIT_POISON, check_confused);

    if (you.get_mutation_level(MUT_BREATHE_MAGMA))
        _add_talent(talents, ABIL_BREATHE_MAGMA, check_confused);

    if ((!form_changed_physiology() || you.form == transformation::dragon)
        && draconian_breath() != ABIL_NON_ABILITY)
    {
        _add_talent(talents, draconian_breath(), check_confused);

        if (you.drac_colour == DR_GOLDEN)
        { 
            _add_talent(talents, ABIL_BREATHE_FROST, check_confused);
            if (you.form == transformation::dragon)
                _add_talent(talents, ABIL_BREATHE_POISON, check_confused);
            else
                _add_talent(talents, ABIL_BREATHE_MEPHITIC, check_confused);
        }
    }

    if (you.racial_permanent_flight() && !you.attribute[ATTR_PERM_FLIGHT] && !you.mounted())
    {
        // Tengu can fly starting at XL 5
        // Draconians and gargoyles get permaflight at XL 14, but they
        // don't get the tengu movement/evasion bonuses
        _add_talent(talents, ABIL_FLY, check_confused);
    }

    if (you.get_mutation_level(MUT_ROOTS) && !you.attribute[ATTR_ROOTED])
        _add_talent(talents, ABIL_PLANT_ROOTS, check_confused);

    if (you.attribute[ATTR_ROOTED])
        _add_talent(talents, ABIL_DEROOT, check_confused);

    if (you.attribute[ATTR_PERM_FLIGHT] && you.racial_permanent_flight())
        _add_talent(talents, ABIL_STOP_FLYING, check_confused);

    // Mutations
    if (you.get_mutation_level(MUT_HURL_HELLFIRE))
        _add_talent(talents, ABIL_HELLFIRE, check_confused);

    if (you.get_mutation_level(MUT_TRANSLUCENT_SKIN) == 3 && !you.duration[DUR_INVIS])
        _add_talent(talents, ABIL_TURN_INVISIBLE, check_confused);

    if (you.get_mutation_level(MUT_BUDDING_EYEBALLS) == 3)
        _add_talent(talents, ABIL_BUD_EYEBALLS, check_confused);

    if (you.get_mutation_level(MUT_JIBBERING_MAWS) == 3)
        _add_talent(talents, ABIL_SILENT_SCREAM, check_confused);

    if (you.get_mutation_level(MUT_FROST_BURST) == 3)
        _add_talent(talents, ABIL_FROST_BURST, check_confused);

    if (you.get_mutation_level(MUT_ACID_WAVE) == 3)
        _add_talent(talents, ABIL_CORROSIVE_WAVE, check_confused);

    if (you.get_mutation_level(MUT_MELT) == 3)
        _add_talent(talents, ABIL_SLIME_BOLT, check_confused);

    if (you.get_mutation_level(MUT_CYTOPLASMIC_SUSPENSION))
        _add_talent(talents, ABIL_SUBSUME, check_confused);

    if (you.equip[EQ_CYTOPLASM] != -1)
        _add_talent(talents, ABIL_EJECT, check_confused);

    if (you.duration[DUR_TRANSFORMATION] && !you.transform_uncancellable)
        _add_talent(talents, ABIL_END_TRANSFORMATION, check_confused);

    if (you.attribute[ATTR_SKELETON])
        _add_talent(talents, ABIL_END_UPRISING, check_confused);

    if (you.mounted())
        _add_talent(talents, ABIL_DISMOUNT, check_confused);

    if (you.get_mutation_level(MUT_BLINK))
        _add_talent(talents, ABIL_BLINK, check_confused);

    // Religious abilities.
    for (ability_type abil : get_god_abilities(include_unusable, false,
                                               include_unusable))
    {
        _add_talent(talents, abil, check_confused);
    }

    // And finally, the ability to opt-out of your faith {dlb}:
    if (!you_worship(GOD_NO_GOD))
        _add_talent(talents, ABIL_RENOUNCE_RELIGION, check_confused);

    if (env.level_state & LSTATE_BEOGH && can_convert_to_beogh())
        _add_talent(talents, ABIL_CONVERT_TO_BEOGH, check_confused);

    if (you.species != SP_DRACONIAN && you.form == transformation::dragon)
        _add_talent(talents, ABIL_BREATHE_FIRE, check_confused);

    if (you.species == SP_DRACONIAN && you.form == transformation::statue)
        _add_talent(talents, ABIL_BREATHE_METAL, check_confused);

    if (you.duration[DUR_PORTAL_PROJECTILE])
        _add_talent(talents, ABIL_CANCEL_PPROJ, check_confused);

    const item_def * inside = you.slot_item(EQ_CYTOPLASM);

    // Evocations from items.
    if ((you.scan_artefacts(ARTP_BLINK) || (inside && get_weapon_brand(*inside) == SPWPN_DISTORTION))
        && !you.get_mutation_level(MUT_NO_ARTIFICE))
    {
        _add_talent(talents, ABIL_EVOKE_BLINK, check_confused);
    }

    if (player_equip_unrand(UNRAND_THIEF)
        && !you.get_mutation_level(MUT_NO_ARTIFICE))
    {
        _add_talent(talents, ABIL_EVOKE_FOG, check_confused);
    }

    if (player_equip_unrand(UNRAND_RATSKIN_CLOAK)
        && !you.get_mutation_level(MUT_NO_ARTIFICE)
        && !you.get_mutation_level(MUT_NO_LOVE))
    {
        _add_talent(talents, ABIL_EVOKE_RATSKIN, check_confused);
    }

    if (player_equip_unrand(UNRAND_RCLOUDS)
        && !you.get_mutation_level(MUT_NO_ARTIFICE))
    {
        _add_talent(talents, ABIL_EVOKE_THUNDER, check_confused);
    }

    if (you.evokable_berserk() && !you.get_mutation_level(MUT_NO_ARTIFICE))
        _add_talent(talents, ABIL_EVOKE_BERSERK, check_confused);

    if (you.evokable_invis() > 0
        && !you.get_mutation_level(MUT_NO_ARTIFICE)
        && !you.duration[DUR_INVIS])
    {
        _add_talent(talents, ABIL_EVOKE_TURN_INVISIBLE, check_confused);
    }

    if (you.evokable_flight() && !you.get_mutation_level(MUT_NO_ARTIFICE))
    {
        // Has no effect on permanently flying Tengu.
        if (!you.permanent_flight() || !you.racial_permanent_flight())
        {
            // You can still evoke perm flight if you have temporary one.
            if (!you.airborne()
                || !you.attribute[ATTR_PERM_FLIGHT])
            {
                _add_talent(talents, ABIL_EVOKE_FLIGHT, check_confused);
            }
            // Now you can only turn flight off if you have an
            // activatable item. Potions and spells will have to time
            // out.
            if (you.airborne() && !you.attribute[ATTR_FLIGHT_UNCANCELLABLE])
                _add_talent(talents, ABIL_STOP_FLYING, check_confused);
        }
    }

    // Mount-based Talents (currently only Spider Mount has any)
    if (you.mounted() && (you.mount == mount_type::spider))
    {
        _add_talent(talents, ABIL_SPIDER_JUMP, check_confused);
        _add_talent(talents, ABIL_SPIDER_WEB, check_confused);
    }

    // Find hotkeys for the non-hotkeyed talents.
    for (talent &tal : talents)
    {
        const int index = _lookup_ability_slot(tal.which);
        if (index > -1)
        {
            tal.hotkey = index_to_letter(index);
            continue;
        }

        // Try to find a free hotkey for i, starting from Z.
        for (int k = 51; k >= 0; --k)
        {
            const int kkey = index_to_letter(k);
            bool good_key = true;

            // Check that it doesn't conflict with other hotkeys.
            for (const talent &other : talents)
                if (other.hotkey == kkey)
                {
                    good_key = false;
                    break;
                }

            if (good_key)
            {
                tal.hotkey = kkey;
                you.ability_letter_table[k] = tal.which;
                break;
            }
        }
        // In theory, we could be left with an unreachable ability
        // here (if you have 53 or more abilities simultaneously).
    }

    return talents;
}

/**
 * Maybe move an ability to the slot given by the ability_slot option.
 *
 * @param[in] slot current slot of the ability
 * @returns the new slot of the ability; may still be slot, if the ability
 *          was not reassigned.
 */
int auto_assign_ability_slot(int slot)
{
    const ability_type abil_type = you.ability_letter_table[slot];
    const string abilname = lowercase_string(ability_name(abil_type));
    bool overwrite = false;
    // check to see whether we've chosen an automatic label:
    for (auto& mapping : Options.auto_ability_letters)
    {
        if (!mapping.first.matches(abilname))
            continue;
        for (char i : mapping.second)
        {
            if (i == '+')
                overwrite = true;
            else if (i == '-')
                overwrite = false;
            else if (isaalpha(i))
            {
                const int index = letter_to_index(i);
                ability_type existing_ability = you.ability_letter_table[index];

                if (existing_ability == ABIL_NON_ABILITY
                    || existing_ability == abil_type)
                {
                    // Unassigned or already assigned to this ability.
                    you.ability_letter_table[index] = abil_type;
                    if (slot != index)
                        you.ability_letter_table[slot] = ABIL_NON_ABILITY;
                    return index;
                }
                else if (overwrite)
                {
                    const string str = lowercase_string(ability_name(existing_ability));
                    // Don't overwrite an ability matched by the same rule.
                    if (mapping.first.matches(str))
                        continue;
                    you.ability_letter_table[slot] = abil_type;
                    swap_ability_slots(slot, index, true);
                    return index;
                }
                // else occupied, continue to the next mapping.
            }
        }
    }
    return slot;
}

// Returns an index (0-51) if already assigned, -1 if not.
static int _lookup_ability_slot(const ability_type abil)
{
    // Placeholder handling, part 2: The ability we have might
    // correspond to a placeholder, in which case the ability letter
    // table will contain that placeholder. Convert the latter to
    // its corresponding ability before comparing the two, so that
    // we'll find the placeholder's index properly.
    for (int slot = 0; slot < 52; slot++)
        if (fixup_ability(you.ability_letter_table[slot]) == abil)
            return slot;
    return -1;
}

// Assign a new ability slot if necessary. Returns an index (0-51) if
// successful, -1 if you should just use the next one.
int find_ability_slot(const ability_type abil, char firstletter)
{
    // If we were already assigned a slot, use it.
    int had_slot = _lookup_ability_slot(abil);
    if (had_slot > -1)
        return had_slot;

    // No requested slot, find new one and make it preferred.

    // firstletter defaults to 'f', because a-e is for invocations
    int first_slot = letter_to_index(firstletter);

    ASSERT(first_slot < 52);

    switch (abil)
    {
    case ABIL_ELYVILON_LIFESAVING:
        first_slot = letter_to_index('p');
        break;
    case ABIL_KIKU_GIFT_NECRONOMICON:
        first_slot = letter_to_index('N');
        break;
    case ABIL_TIAMAT_DRAGON_BOOK:
        first_slot = letter_to_index('D');
        break;
    case ABIL_TSO_BLESS_WEAPON:
    case ABIL_KIKU_BLESS_WEAPON:
    case ABIL_LUGONU_BLESS_WEAPON:
    case ABIL_BAHAMUT_DRAGONSLAYING:
        first_slot = letter_to_index('W');
        break;
    case ABIL_CONVERT_TO_BEOGH:
        first_slot = letter_to_index('Y');
        break;
    case ABIL_BAHAMUT_PROTECTION:
    case ABIL_CHOOSE_BAHAMUT_BREATH:
    case ABIL_CHOOSE_BAHAMUT_DRAKE:
    case ABIL_BAHAMUT_TRANSFORM:
        first_slot = letter_to_index('B');
        break;
    case ABIL_TIAMAT_RETRIBUTION:
    case ABIL_CHOOSE_TIAMAT_BREATH:
    case ABIL_CHOOSE_TIAMAT_DRAKE:
    case ABIL_CHOOSE_TIAMAT_TRANSFORM:
        first_slot = letter_to_index('T');
        break;
    case ABIL_RU_SACRIFICE_PURITY:
    case ABIL_RU_SACRIFICE_WORDS:
    case ABIL_RU_SACRIFICE_DRINK:
    case ABIL_RU_SACRIFICE_ESSENCE:
    case ABIL_RU_SACRIFICE_HEALTH:
    case ABIL_RU_SACRIFICE_STEALTH:
    case ABIL_RU_SACRIFICE_ARTIFICE:
    case ABIL_RU_SACRIFICE_LOVE:
    case ABIL_RU_SACRIFICE_COURAGE:
    case ABIL_RU_SACRIFICE_ARCANA:
    case ABIL_RU_SACRIFICE_NIMBLENESS:
    case ABIL_RU_SACRIFICE_DURABILITY:
    case ABIL_RU_SACRIFICE_HAND:
    case ABIL_RU_SACRIFICE_EXPERIENCE:
    case ABIL_RU_SACRIFICE_SKILL:
    case ABIL_RU_SACRIFICE_EYE:
    case ABIL_RU_SACRIFICE_RESISTANCE:
    case ABIL_RU_REJECT_SACRIFICES:
    case ABIL_HEPLIAKLQANA_TYPE_KNIGHT:
    case ABIL_HEPLIAKLQANA_TYPE_BATTLEMAGE:
    case ABIL_HEPLIAKLQANA_TYPE_HEXER:
    case ABIL_HEPLIAKLQANA_IDENTITY: // move this?
        first_slot = letter_to_index('G');
        break;
    default:
        break;
    }

    for (int slot = first_slot; slot < 52; ++slot)
    {
        if (you.ability_letter_table[slot] == ABIL_NON_ABILITY)
        {
            you.ability_letter_table[slot] = abil;
            return auto_assign_ability_slot(slot);
        }
    }

    // If we can't find anything else, try a-e.
    for (int slot = first_slot - 1; slot >= 0; --slot)
    {
        if (you.ability_letter_table[slot] == ABIL_NON_ABILITY)
        {
            you.ability_letter_table[slot] = abil;
            return auto_assign_ability_slot(slot);
        }
    }

    // All letters are assigned.
    return -1;
}


vector<ability_type> get_god_abilities(bool ignore_silence, bool ignore_piety,
                                       bool ignore_penance)
{
    vector<ability_type> abilities;
    if (you_worship(GOD_RU))
    {
        ASSERT(you.props.exists(AVAILABLE_SAC_KEY));
        bool any_sacrifices = false;
        for (const auto& store : you.props[AVAILABLE_SAC_KEY].get_vector())
        {
            any_sacrifices = true;
            abilities.push_back(static_cast<ability_type>(store.get_int()));
        }
        if (any_sacrifices)
            abilities.push_back(ABIL_RU_REJECT_SACRIFICES);
        if (silenced(you.pos()))
        {
            if (piety_rank() >= 3)
                abilities.push_back(ABIL_RU_DRAW_OUT_POWER);
            if (piety_rank() >= 4)
                abilities.push_back(ABIL_RU_POWER_LEAP);
            if (piety_rank() >= 5)
                abilities.push_back(ABIL_RU_APOCALYPSE);
        }
    }

    // XXX: should we check ignore_piety?
    if (you_worship(GOD_HEPLIAKLQANA)
        && piety_rank() >= 2 && !you.props.exists(HEPLIAKLQANA_ALLY_TYPE_KEY))
    {
        for (int anc_type = ABIL_HEPLIAKLQANA_FIRST_TYPE;
             anc_type <= ABIL_HEPLIAKLQANA_LAST_TYPE;
             ++anc_type)
        {
            abilities.push_back(static_cast<ability_type>(anc_type));
        }
    }
    if (you.transfer_skill_points > 0)
        abilities.push_back(ABIL_ASHENZARI_END_TRANSFER);
    if (silenced(you.pos()) && you_worship(GOD_WU_JIAN) && piety_rank() >= 2)
        abilities.push_back(ABIL_WU_JIAN_WALLJUMP);

    if (!ignore_silence && silenced(you.pos()) && !you.can_silent_cast())
        return abilities;
    // Remaining abilities are unusable if silenced.
    for (const auto& power : get_god_powers(you.religion))
    {
        if (god_power_usable(power, ignore_piety, ignore_penance))
        {
            const ability_type abil = fixup_ability(power.abil);
            ASSERT(abil != ABIL_NON_ABILITY);
            abilities.push_back(abil);
        }
    }

    return abilities;
}

void swap_ability_slots(int index1, int index2, bool silent)
{
    // Swap references in the letter table.
    ability_type tmp = you.ability_letter_table[index2];
    you.ability_letter_table[index2] = you.ability_letter_table[index1];
    you.ability_letter_table[index1] = tmp;

    if (!silent)
    {
        mprf_nocap("%c - %s", index_to_letter(index2),
                   ability_name(you.ability_letter_table[index2]));
    }

}

/**
 * What skill affects the success chance/power of a given skill, if any?
 *
 * @param ability       The ability in question.
 * @return              The skill that governs the ability, or SK_NONE.
 */
skill_type abil_skill(ability_type ability)
{
    ASSERT(ability != ABIL_NON_ABILITY);
    return get_ability_def(ability).failure.skill();
}

/**
 * How valuable is it to train the skill that governs this ability? (What
 * 'magnitude' does the ability have?)
 *
 * @param ability       The ability in question.
 * @return              A 'magnitude' for the ability, probably < 10.
 */
int abil_skill_weight(ability_type ability)
{
    ASSERT(ability != ABIL_NON_ABILITY);
    // This is very loosely modelled on a legacy model; fairly arbitrary.
    const int base_fail = get_ability_def(ability).failure.base_chance;
    const int floor = base_fail ? 1 : 0;
    return max(floor, div_rand_round(base_fail, 8) - 3);
}

// When a mutation replaces an ability with a different that should logically
// share the same slot; this tries to maintain the slot in the ability table.
void abil_swap(ability_type old_abil, ability_type new_abil)
{
    for (int i = 0; i < 52; ++i)
        if (you.ability_letter_table[i] == old_abil)
            you.ability_letter_table[i] = new_abil;
}

////////////////////////////////////////////////////////////////////////
// scaling_cost

int scaling_cost::cost(int max) const
{
    return (value < 0) ? (-value) : ((value * max + 500) / 1000);
}
