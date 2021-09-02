/**
 * @file
 * @brief Functions for handling player mutations.
**/

#include "AppHdr.h"

#include "mutation.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "ability.h"
#include "cio.h"
#include "coordit.h"
#include "dactions.h"
#include "delay.h"
#include "english.h"
#include "env.h"
#include "food.h"
#include "god-abil.h"
#include "god-passive.h"
#include "hints.h"
#include "item-prop.h"
#include "item-use.h" // can_wear_armour()
#include "items.h"
#include "libutil.h"
#include "macro.h" // get_ch()
#include "menu.h"
#include "message.h"
#include "mon-place.h"
#include "notes.h"
#include "output.h"
#include "player-equip.h" // lose_permafly_source
#include "player-stats.h"
#include "religion.h"
#include "skills.h"
#include "species_mutation_messaging.h"
#include "state.h"
#include "stringutil.h"
#include "transform.h"
#include "unicode.h"
#include "xom.h"

using namespace ui;

static void _skill_rescale();
static bool _delete_single_mutation_level(mutation_type mutat, const string &reason, bool transient, bool innate = false);
static bool _post_loss_effects(mutation_type mutat, bool temp = false);
static void _transpose_gear();
static void _return_gear();

struct body_facet_def
{
    equipment_type eq;
    mutation_type mut;
};

struct facet_def
{
    int tier;
    mutation_type muts[3];
    int when[3];
};

struct demon_mutation_info
{
    mutation_type mut;
    int when;
    int facet;

    demon_mutation_info(mutation_type m, int w, int f)
        : mut(m), when(w), facet(f) { }
};

enum class mutflag
{
    good    = 1 << 0, // used by benemut etc
    bad     = 1 << 1, // used by malmut etc
    jiyva   = 1 << 2, // jiyva-only muts
    qazlal  = 1 << 3, // qazlal wrath
    xom     = 1 << 4, // xom being xom

    last    = xom
};
DEF_BITFIELD(mutflags, mutflag, 4);
COMPILE_CHECK(mutflags::exponent(mutflags::last_exponent) == mutflag::last);

#include "mutation-data.h"

static const body_facet_def _body_facets[] =
{
    //{ EQ_NONE, MUT_FANGS },
    { EQ_HELMET, MUT_HORNS },
    { EQ_HELMET, MUT_ANTENNAE },
    //{ EQ_HELMET, MUT_BEAK },
    { EQ_GLOVES, MUT_CLAWS },
    { EQ_BOOTS, MUT_HOOVES },
    { EQ_BOOTS, MUT_TALONS },
    { EQ_BOOTS, MUT_FROG_LEGS }
};

/**
 * Conflicting mutation pairs. Entries are symmetric (so if A conflicts
 * with B, B conflicts with A in the same way).
 *
 * The third value in each entry means:
 *   0: If the new mutation is forced, remove all levels of the old
 *      mutation. Either way, keep scanning for more conflicts and
 *      do what they say (accepting the mutation if there are no
 *      further conflicts).
 *
 *  -1: If the new mutation is forced, remove all levels of the old
 *      mutation and scan for more conflicts. If it is not forced,
 *      fail at giving the new mutation.
 *
 *   1: If the new mutation is temporary, just allow the conflict.
 *      Otherwise, trade off: delete one level of the old mutation,
 *      don't give the new mutation, and consider it a success.
 *
 * It makes sense to have two entries for the same pair, one with value 0
 * and one with 1: that would replace all levels of the old mutation if
 * forced, or a single level if not forced. However, the 0 entry must
 * precede the 1 entry; so if you re-order this list, keep all the 0s
 * before all the 1s.
 */
static const int conflict[][3] =
{
    { MUT_REGENERATION,                 MUT_INHIBITED_REGENERATION,          0},
    { MUT_ACUTE_VISION,                 MUT_IMPAIRED_VISION,                 0},
    { MUT_FAST,                         MUT_SLOW,                            0},
    { MUT_ROBUST,                       MUT_FRAIL,                           1},
    { MUT_HIGH_MAGIC,                   MUT_LOW_MAGIC,                       1},
    { MUT_WILD_MAGIC,                   MUT_SUBDUED_MAGIC,                   1},
    { MUT_CARNIVOROUS,                  MUT_HERBIVOROUS,                     1},
    { MUT_SLOW_METABOLISM,              MUT_FAST_METABOLISM,                 1},
    { MUT_REGENERATION,                 MUT_INHIBITED_REGENERATION,          1},
    { MUT_ACUTE_VISION,                 MUT_IMPAIRED_VISION,                 1},
    { MUT_BERSERK,                      MUT_CLARITY,                         1},
    { MUT_SILENT_CAST,                  MUT_SHOUTITUS                       -1},
    { MUT_FAST,                         MUT_SLOW,                            1},
    { MUT_FANGS,                        MUT_BEAK,                           -1},
    { MUT_ANTENNAE,                     MUT_HORNS,                          -1}, // currently overridden by physiology_mutation_conflict
    { MUT_HOOVES,                       MUT_TALONS,                         -1}, // currently overridden by physiology_mutation_conflict
    { MUT_HOOVES,                       MUT_FROG_LEGS,                      -1}, // currently overridden by physiology_mutation_conflict
    { MUT_TALONS,                       MUT_FROG_LEGS,                      -1}, // currently overridden by physiology_mutation_conflict
    { MUT_SILENCE_AURA,                 MUT_SHOUTITUS,                      -1},
    { MUT_MUTATION_RESISTANCE,          MUT_EVOLUTION,                      -1},
    { MUT_ANTIMAGIC_BITE,               MUT_ACIDIC_BITE,                    -1},
    { MUT_NEGATIVE_ENERGY_RESISTANCE,   MUT_NEGATIVE_ENERGY_VULNERABILITY,  -1},
    { MUT_ACID_RESISTANCE,              MUT_ACID_VULNERABILITY,             -1},
    { MUT_ACID_RESISTANCE,              MUT_SLIME,                          -1},
    { MUT_ACID_VULNERABILITY,           MUT_SLIME,                          -1},
    { MUT_HEAT_RESISTANCE,              MUT_HEAT_VULNERABILITY,             -1},
    { MUT_COLD_RESISTANCE,              MUT_COLD_VULNERABILITY,             -1},
    { MUT_SHOCK_RESISTANCE,             MUT_SHOCK_VULNERABILITY,            -1},
    { MUT_MAGIC_RESISTANCE,             MUT_MAGICAL_VULNERABILITY,          -1},
    { MUT_NO_REGENERATION,              MUT_INHIBITED_REGENERATION,         -1},
    { MUT_NO_REGENERATION,              MUT_REGENERATION,                   -1},
    { MUT_FORLORN,                      MUT_GODS_PITY,                       1},
    { MUT_NIGHTSTALKER,                 MUT_DAYSTRIDER,                      0},
    { MUT_NIGHTSTALKER,                 MUT_DAYSTRIDER,                      1},
};

equipment_type beastly_slot(int mut)
{
    switch (mut)
    {
    case MUT_HORNS:
    case MUT_ANTENNAE:
    // Not putting MUT_BEAK here because it doesn't conflict with the other two.
        return EQ_HELMET;
    case MUT_CLAWS:
        return EQ_GLOVES;
    case MUT_HOOVES:
    case MUT_TALONS:
        return EQ_BOOTS;
    case MUT_TENTACLE_SPIKE:
        return EQ_CLOAK;
    default:
        return EQ_NONE;
    }
}

static bool _mut_has_use(const mutation_def &mut, mutflag use)
{
    return bool(mut.uses & use);
}

#define MUT_BAD(mut) _mut_has_use((mut), mutflag::bad)
#define MUT_GOOD(mut) _mut_has_use((mut), mutflag::good)

static int _suppress_weight(bool temp)
{
    return you.how_mutated(temp ? false : true, true, false, true, temp ? true : false)
        * (temp && you.char_class == JOB_DEMONSPAWN ? 2 : 1);
}

static int _mut_weight(const mutation_def &mut, mutflag use, bool temp = false)
{
    switch (use)
    {
        case mutflag::jiyva:
        case mutflag::qazlal:
        case mutflag::xom:
            return 1;
        case mutflag::bad:
            if (mut.mutation == MUT_SUPPRESSION)
                return _suppress_weight(temp);
            // fallthrough
        case mutflag::good:
        default:
            return mut.weight;
    }
}

static int mut_index[NUM_MUTATIONS];
static int category_mut_index[MUT_NON_MUTATION - CATEGORY_MUTATIONS];
static map<mutflag, int> total_weight;

void init_mut_index()
{
    total_weight.clear();
    for (int i = 0; i < NUM_MUTATIONS; ++i)
        mut_index[i] = -1;

    for (unsigned int i = 0; i < ARRAYSZ(mut_data); ++i)
    {
        const mutation_type mut = mut_data[i].mutation;
        ASSERT_RANGE(mut, 0, NUM_MUTATIONS);
        ASSERT(mut_index[mut] == -1);
        mut_index[mut] = i;

        if (mut == MUT_SUPPRESSION) // Weight handled specially.
            continue;

        for (const auto flag : mutflags::range())
        {
            if (_mut_has_use(mut_data[i], flag))
                total_weight[flag] += _mut_weight(mut_data[i], flag);
        }
    }

    // this is all a bit silly but ok
    for (int i = 0; i < MUT_NON_MUTATION - CATEGORY_MUTATIONS; ++i)
        category_mut_index[i] = -1;

    for (unsigned int i = 0; i < ARRAYSZ(category_mut_data); ++i)
    {
        const mutation_type mut = category_mut_data[i].mutation;
        ASSERT_RANGE(mut, CATEGORY_MUTATIONS, MUT_NON_MUTATION);
        ASSERT(category_mut_index[mut-CATEGORY_MUTATIONS] == -1);
        category_mut_index[mut-CATEGORY_MUTATIONS] = i;
    }
}

static const mutation_def& _get_mutation_def(mutation_type mut)
{
    ASSERT_RANGE(mut, 0, NUM_MUTATIONS);
    ASSERT(mut_index[mut] != -1);
    return mut_data[mut_index[mut]];
}

/*
 * Get the max number of possible levels for mutation `mut`. This is typically 1 or 3.
 *
 * @return the mutation cap.
 */
int get_mutation_cap(mutation_type mut)
{
    return _get_mutation_def(mut).levels;
}

static const mutation_category_def& _get_category_mutation_def(mutation_type mut)
{
    ASSERT_RANGE(mut, CATEGORY_MUTATIONS, MUT_NON_MUTATION);
    ASSERT(category_mut_index[mut-CATEGORY_MUTATIONS] != -1);
    return category_mut_data[category_mut_index[mut-CATEGORY_MUTATIONS]];
}

bool is_valid_mutation(mutation_type mut)
{
    return mut >= 0 && mut < NUM_MUTATIONS && mut_index[mut] != -1;
}

static const mutation_type _all_scales[] =
{
    MUT_DISTORTION_FIELD,           MUT_ICY_BLUE_SCALES,
    MUT_IRIDESCENT_SCALES,          MUT_LARGE_BONE_PLATES,
    MUT_MOLTEN_SCALES,              MUT_ROUGH_BLACK_SCALES,
    MUT_RUGGED_BROWN_SCALES,        MUT_SLIMY_GREEN_SCALES,
    MUT_THIN_METALLIC_SCALES,       MUT_THIN_SKELETAL_STRUCTURE,
    MUT_YELLOW_SCALES,              MUT_STURDY_FRAME,
    MUT_SANGUINE_ARMOUR,
};

static bool _is_covering(mutation_type mut)
{
    return find(begin(_all_scales), end(_all_scales), mut) != end(_all_scales);
}

bool is_body_facet(mutation_type mut)
{
    return any_of(begin(_body_facets), end(_body_facets),
                  [=](const body_facet_def &facet)
                  { return facet.mut == mut; });
}

/*
 * The degree to which `mut` is suppressed by the current form.
 *
 * @param mut  the mutation to check.
 *
 * @return  mutation_activity_type::FULL: completely available.
 *          mutation_activity_type::PARTIAL: partially suppressed.
 *          mutation_activity_type::INACTIVE: completely suppressed.
 */
mutation_activity_type mutation_activity_level(mutation_type mut)
{
    // First make sure the player's form permits the mutation.
    if (!form_keeps_mutations())
    {
        if (you.undead_state() && (mut == MUT_COLD_RESISTANCE || mut == MUT_UNBREATHING)
            && you.innate_mutation[mut])
        {
            return mutation_activity_type::FULL;
        }
        if (you.form == transformation::dragon)
        {
            if (mut == MUT_DRACONIAN_DEFENSE || mut == MUT_DRACONIAN_ENHANCER
                || mut == MUT_ACIDIC_BITE || mut == MUT_STINGER)
            {
                return mutation_activity_type::FULL;
            }
        }
        // Vampire bats keep their fangs.
        if (you.form == transformation::bat
            && you.species == SP_VAMPIRE
            && mut == MUT_FANGS)
        {
            return mutation_activity_type::FULL;
        }
        // Dex and HP changes are kept in all forms.
        if (mut == MUT_RUGGED_BROWN_SCALES || mut == MUT_ROUGH_BLACK_SCALES)
            return mutation_activity_type::PARTIAL;
        else if (_get_mutation_def(mut).form_based)
            return mutation_activity_type::INACTIVE;
    }

    if (you.form == transformation::statue)
    {
        // Statues get all but the AC benefit from scales, but are not affected
        // by other changes in body material or speed.
        switch (mut)
        {
        case MUT_OOZOMORPH:
        case MUT_SLIME:
        case MUT_FAST:
        case MUT_SLOW:
        case MUT_IRIDESCENT_SCALES:
        case MUT_ROUGH_BLACK_SCALES:
            return mutation_activity_type::INACTIVE;
        case MUT_RUGGED_BROWN_SCALES:
        case MUT_YELLOW_SCALES:
        case MUT_ICY_BLUE_SCALES:
        case MUT_MOLTEN_SCALES:
        case MUT_SLIMY_GREEN_SCALES:
        case MUT_THIN_METALLIC_SCALES:
            return mutation_activity_type::PARTIAL;
        default:
            break;
        }
    }

    //XXX: Should this make claws inactive too?
    if (you.form == transformation::blade_hands && mut == MUT_PAWS)
        return mutation_activity_type::INACTIVE;

    if ((you.form == transformation::tree || you.attribute[ATTR_ROOTED])
        && (mut == MUT_BLINK || mut == MUT_TELEPORT))
    {
        return mutation_activity_type::INACTIVE;
    }

#if TAG_MAJOR_VERSION == 34
    if ((you_worship(GOD_PAKELLAS) || player_under_penance(GOD_PAKELLAS))
         && (mut == MUT_MANA_LINK || mut == MUT_MANA_REGENERATION))
    {
        return mutation_activity_type::INACTIVE;
    }
#endif

    if (!form_can_bleed(you.form) && mut == MUT_SANGUINE_ARMOUR)
        return mutation_activity_type::INACTIVE;

    if (mut == MUT_DEMONIC_GUARDIAN && you.get_mutation_level(MUT_NO_LOVE))
        return mutation_activity_type::INACTIVE;

    return mutation_activity_type::FULL;
}

// Counts of various statuses/types of mutations from the current/most
// recent call to describe_mutations. TODO: eliminate
static int _num_full_suppressed = 0;
static int _num_part_suppressed = 0;
static int _num_transient = 0;

static string _annotate_form_based(string desc, bool suppressed)
{
    if (suppressed)
    {
        desc = "<darkgrey>((" + desc + "))</darkgrey>";
        ++_num_full_suppressed;
    }

    return desc + "\n";
}

/*
 * Does the player have mutation `mut` with at least one temporary level?
 *
 * Reminder: temporary mutations can coexist with innate or normal mutations.
 */
bool player::has_temporary_mutation(mutation_type mut) const
{
    return you.temp_mutation[mut] > 0;
}

/*
 * Does the player have mutation `mut` with at least one innate level?
 *
 * Reminder: innate mutations can coexist with temporary or normal mutations.
 */
bool player::has_innate_mutation(mutation_type mut) const
{
    return you.innate_mutation[mut] > 0;
}

/*
 * How much of mutation `mut` does the player have? This ignores form changes.
 * If all three bool arguments are false, this should always return 0.
 *
 * @param temp    -    include temporary mutation levels. defaults to true.
 * @param innate   -   include innate mutation levels. defaults to true.
 * @param normal   -   include normal (non-temp, non-innate) mutation levels. defaults to true.
 * @param suppressed - include suppression (reduction of innates due to mutation; defaults to true).
 *
 * @return the total levels of the mutation.
 */
int player::get_base_mutation_level(mutation_type mut, bool innate, bool temp, bool normal, bool suppressed) const
{
    ASSERT_RANGE(mut, 0, NUM_MUTATIONS);
    // you.mutation stores the total levels of all mutations
    int level = you.mutation[mut];
    if (!temp)
        level -= you.temp_mutation[mut];
    if (!innate)
        level -= you.innate_mutation[mut];
    if (!normal)
    {
        level -= you.mutation[mut];
        level += you.temp_mutation[mut] + you.innate_mutation[mut];
    }
    if (suppressed)
        level -= you.suppressed_mutation[mut] ? 1 : 0;
    
    return level;
}

/*
 * How much of mutation `mut` does the player have innately?
 *
 */
int player::get_innate_mutation_level(mutation_type mut) const
{
    ASSERT_RANGE(mut, 0, NUM_MUTATIONS);
    return you.innate_mutation[mut];
}

/*
 * How much of mutation `mut` does the player have temporarily?
 *
 */
int player::get_temp_mutation_level(mutation_type mut) const
{
    ASSERT_RANGE(mut, 0, NUM_MUTATIONS);
    return you.temp_mutation[mut];
}

/*
 * Get the current player mutation level for `mut`, possibly incorporating information about forms.
 * See the other version of this function for the canonical usage of `minact`; some forms such as scale mutations
 * have different thresholds depending on the purpose and form and so will call this directly (e.g. ac
 * but not resistances are suppressed in statueform.)
 *
 * @param mut           the mutation to check
 * @param minact        the minimum activity level needed for the mutation to count as non-suppressed.
 *
 * @return a mutation level, 0 if the mutation doesn't exist or is suppressed.
 */
int player::get_mutation_level(mutation_type mut, mutation_activity_type minact) const
{
    ASSERT_RANGE(mut, 0, NUM_MUTATIONS);
    if (!is_valid_mutation(mut))
        return 0;
    if (mutation_activity_level(mut) < minact)
        return 0;
    return get_base_mutation_level(mut, true, true);
}

/*
 * Get the current player mutation level for `mut`, possibly incorporating information about forms.
 *
 * @param mut           the mutation to check
 * @param check_form    whether to incorporate suppression from forms. Defaults to true.
 *
 * @return a mutation level, 0 if the mutation doesn't exist or is suppressed.
 */
int player::get_mutation_level(mutation_type mut, bool check_form) const
{
    return get_mutation_level(mut, check_form ? mutation_activity_type::PARTIAL :
                                                            mutation_activity_type::INACTIVE);
}

/*
 * Does the player have mutation `mut` in some form?
 */
bool player::has_mutation(mutation_type mut, bool check_form) const
{
    return get_mutation_level(mut, check_form) > 0;
}

/*
 * Test the validity of the player mutation structures, using ASSERTs.
 * Will crash on a failure.
 *
 * @debug_msg whether to output diagnostic `dprf`s in the process.
 */
void validate_mutations(bool debug_msg)
{
    if (debug_msg)
        dprf("Validating player mutations");
    int total_temp = 0;

    for (int i = 0; i < NUM_MUTATIONS; i++)
    {
        mutation_type mut = static_cast<mutation_type>(i);

        if (!is_valid_mutation(mut))
            continue;
        
        if (debug_msg && you.mutation[mut] > 0)
        {
            dprf("mutation %s: total %d innate %d temp %d",
                mutation_name(mut), you.mutation[mut],
                you.innate_mutation[mut], you.temp_mutation[mut]);
        }
        ASSERT(you.get_base_mutation_level(mut) == you.mutation[mut]);
        ASSERT(you.mutation[i] >= you.innate_mutation[mut] + you.temp_mutation[mut]);
        total_temp += you.temp_mutation[mut];

        const mutation_def& mdef = _get_mutation_def(mut);
        ASSERT(you.mutation[mut] <= mdef.levels);

        // reconstruct what the innate mutations should be based on Ds mutation schedule
        // TODO generalize to all innate muts
        if (you.species == SP_DEMONSPAWN || you.char_class == JOB_DEMONSPAWN)
        {
            bool is_trait = false;
            int trait_level = 0;
            // If the player has sacrificed xp, use the pre-sac xl; sac xp
            // doesn't remove Ds mutations.
            // You can still trick wizmode into crashing here.
            const int check_xl = (you.get_mutation_level(MUT_INEXPERIENCED)
                            && you.max_level <= you.get_experience_level() + 2)
                                ? you.max_level
                                : you.get_experience_level();
            for (player::demon_trait trait : you.demonic_traits)
            {
                if (trait.mutation == mut)
                {
                    is_trait = true;
                    if (check_xl >= trait.level_gained)
                        trait_level += 1;
                }
            }

            if (debug_msg && is_trait)
            {
                dprf("scheduled innate for %s: %d, actual %d", mutation_name(mut),
                     trait_level, you.innate_mutation[mut]);
            }
            if (is_trait)
                ASSERT(you.innate_mutation[mut] == trait_level);
        }
    }
    ASSERT(total_temp == you.attribute[ATTR_TEMP_MUTATIONS]);
}

string describe_breath(bool gain)
{
    ostringstream ostr;
    if (gain)
        ostr << "You lose your ability to breathe basic magical darts and instead you can breathe ";
    else
        ostr << "You can breathe ";

    if (you.form == transformation::statue)
    {
        ostr << "metallic splinters.";
        return ostr.str();
    }

    switch (you.drac_colour)
    {
    case DR_BROWN:          ostr << "rudimentary magical darts.";   break;
    case DR_BLOOD:          ostr << "clouds of vampiric fog.";      break;
    case DR_BLUE:           ostr << "wild blasts of lightning";     break;
    case DR_BONE:           ostr << "shards of bone.";              break;
    case DR_BLACK:          ostr << "bolts of negative energy.";    break;
    case DR_CYAN:           ostr << "strong gales of wind.";        break;
    case DR_GREEN:          ostr << "clouds of noxious fumes.";     break;
    case DR_LIME:           ostr << "balls of acidic spit.";        break;
    case DR_MAGENTA:        ostr << "clouds of thick fog.";         break;
    case DR_OLIVE:          ostr << "clouds of foul miasma.";       break;
    case DR_PEARL:          ostr << "bursts of cleansing flame.";   break;
    case DR_PINK:           ostr << "friendly butterflies.";        break;
    case DR_PLATINUM:       ostr << "blasts of radiation.";         break;
    case DR_PURPLE:         ostr << "bolts of dispelling energy.";  break;
    case DR_WHITE:          ostr << "puffs of frost.";              break;
    case DR_SCINTILLATING:  ostr << "bolts of chaotic energy.";     break;
    case DR_GOLDEN:         ostr << "your choice of flames, frost or noxious fumes.";                           break;
    case DR_RED:            ostr << "puffs of flames, which leave flaming clouds in their wake.";               break;
    case DR_SILVER:         ostr << "silver splinters, which deal bonus damage to chaotic creatures.";          break;
    case DR_TEAL:           ostr << "blasts of ghostly flames, which heal the undead and damage the living.";   break;
    default:                ostr << "buggy bugs of bugging";        break;
    }

    return ostr.str();
}

string describe_mutations(bool drop_title)
{
#ifdef DEBUG
    validate_mutations(true);
#endif
    string result;

    _num_full_suppressed = _num_part_suppressed = 0;
    _num_transient = 0;

    if (!drop_title)
    {
        result += "<white>";
        result += "Innate Abilities, Weirdness & Mutations";
        result += "</white>\n\n";
    }

    result += "<lightblue>";
    const string old_result = result;

    // Innate abilities which haven't been implemented as mutations yet.
    for (const string& str : fake_mutations(you.species, false))
        result += str + "\n";

    if (you.racial_ac(false) >= 100)
    {             
        const string job_string = you.char_class == JOB_CENTAUR ? "horsehide" : 
                                  you.char_class == JOB_NAGA    ? "serpentine skin" : "";

        const string race_string = you.species == SP_DRACONIAN ? string(scale_type()) + " scales" :
                                   you.species == SP_NAGA      ? "serpentine skin"                :
                                   you.species == SP_GARGOYLE  ? "stone body"                     :
                                   you.species == SP_TROLL     ? (job_string.length() ? "tough skin, shaggy fur" : "tough skin and shaggy fur") :
                                   you.species == SP_CENTAUR   ? "horsehide"                      :
                                   you.species == SP_OGRE      ? "tough skin"                     :
                                   you.species == SP_LIGNIFITE ? "bark"                           : 
                                   you.species == SP_FELID     ? "fur coat"                       :
                                   you.species == SP_OCTOPODE  ? "gelatinous body"                : "";

        const bool plural = race_string.length() && job_string.length();

        result += _annotate_form_based(
            make_stringf("Your %s%s%s %s resilient. (AC +%d%s)", race_string.c_str(),
                plural ? " and " : "",
                job_string.c_str(), plural || you.species == SP_DRACONIAN ? "are" : "is",
                you.racial_ac(false) / 100,
                you.species == SP_OCTOPODE ? ", EV +1" : ""),
            player_is_shapechanged()
            && !(species_is_draconian(you.species)
                && you.form == transformation::dragon));
    }

    if (you.species == SP_OCTOPODE)
    {
        result += _annotate_form_based("You are amphibious.",
            !form_likes_water());

        int tents = you.usable_tentacles(false) + you.num_constricting();

        if (you.have_serpentine_tail() && you.num_constricting())
            tents--;

        const string num_tentacles = number_in_words(tents);
        const string total_tentacles = number_in_words(you.has_tentacles(false));
        result += _annotate_form_based(
            make_stringf("You can wear up to %s rings at the same time.",
                total_tentacles.c_str()),
            !get_form()->slot_available(EQ_RING_FOUR));
        result += _annotate_form_based(
            make_stringf("You can use your tentacles to constrict %s enemies at once.",
                num_tentacles.c_str()),
            !form_keeps_mutations());
    }

    if (you.species != SP_FELID && you.species != SP_FAIRY)
    {
        switch (you.body_size(PSIZE_TORSO, true))
        {
        case SIZE_LITTLE:
            result += "You are very small and have problems with some larger weapons.\n"
                "You are too small for most types of armour.\n";
            break;
        case SIZE_SMALL:
            result += "You are small and have problems with some larger weapons.\n";
            break;
        case SIZE_LARGE:
            result += "You are too large for most types of armour.\n";
            break;
        case SIZE_GIANT:
            result += "Your enormous size allows you to wade in deep water.\n"
                "You are too large for most types of armour.\n"
                "Webs and nets cannot contain a creature of your size.\n";
            break;
        default:
            break;
        }
    }

    if (you.species == SP_DRACONIAN)
    {
        if (you.undead_state(false) == US_UNDEAD)
        {
            result += "Your connection to dragons allows you to use the spell dragon form,\n"
                "despite your undead form not otherwise being capable of transmutation.\n";
        }
        result += _annotate_form_based(describe_breath(), 
                (!form_keeps_mutations() && you.form != transformation::dragon 
                                         && you.form != transformation::lich));
    }

    if (player_res_poison(false, false, false) == 3)
        result += "You are immune to poison.\n";

    result += "</lightblue>";

    // First add (non-removable) inborn abilities and demon powers.
    for (int i = 0; i < NUM_MUTATIONS; i++)
    {
        mutation_type mut_type = static_cast<mutation_type>(i);

        if (!is_valid_mutation(mut_type))
            continue;

        if (you.has_innate_mutation(mut_type))
        {
            if (you.char_class == JOB_CENTAUR && mut_type == MUT_TALONS)
                continue;

            result += mutation_desc(mut_type, -1, true,
                ((you.sacrifices[i] != 0) ? true : false));
            result += "\n";
        }
    }

    if (have_passive(passive_t::water_walk))
        result += "<green>You can walk on water.</green>\n";
    else if (you.can_water_walk())
    {
        result += "<lightgreen>You can walk on water until reaching land."
            "</lightgreen>";
    }

    if (have_passive(passive_t::frail)
        || player_under_penance(GOD_HEPLIAKLQANA))
    {
        result += "<lightred>Your life essence is reduced to manifest your ancestor. (-10% HP)"
            "</lightred>\n";
    }

    if (have_passive(passive_t::berserkitis)
        || player_under_penance(GOD_TROG))
    {
        result += "<lightred>You frequently lose your temper in combat. This godly rage surpasses even clarity. (*Rage)</lightred>\n";
    }

    // Now add removable mutations.
    for (int i = 0; i < NUM_MUTATIONS; i++)
    {
        mutation_type mut_type = static_cast<mutation_type>(i);

        if (!is_valid_mutation(mut_type))
            continue;

        if (you.get_base_mutation_level(mut_type, false, false, true) > 0
            && !you.has_innate_mutation(mut_type) && !you.has_temporary_mutation(mut_type))
        {
            result += mutation_desc(mut_type, -1, true);
            result += "\n";
        }
    }

    //Finally, temporary mutations.
    for (int i = 0; i < NUM_MUTATIONS; i++)
    {
        mutation_type mut_type = static_cast<mutation_type>(i);

        if (!is_valid_mutation(mut_type))
            continue;

        if (you.has_temporary_mutation(mut_type))
        {
            result += mutation_desc(mut_type, -1, true);
            result += "\n";
        }
    }

    if (result == old_result + "</lightblue>") // Nothing was added
        result += "You are rather mundane.\n";

    return result;
}

static formatted_string _vampire_Ascreen_footer(bool first_page)
{
    const char *text = first_page ? "<w>Mutations</w>|Blood properties"
                                  : "Mutations|<w>Blood properties</w>";
    const string fmt = make_stringf("[<w>!</w>/<w>^</w>"
#ifdef USE_TILE_LOCAL
            "|<w>Right-click</w>"
#endif
            "]: %s", text);
    return formatted_string::parse_string(fmt);
}

static int _vampire_bloodlessness()
{
    switch (you.hunger_state)
    {
    case HS_ENGORGED:
    case HS_VERY_FULL:
    case HS_FULL:
        return 1;
    case HS_SATIATED:
        return 2;
    case HS_HUNGRY:
    case HS_VERY_HUNGRY:
    case HS_NEAR_STARVING:
        return 3;
    case HS_STARVING:
    case HS_FAINTING:
        return 4;
    }
    die("bad hunger state %d", you.hunger_state);
}

static string _display_vampire_attributes()
{
    ASSERT(you.species == SP_VAMPIRE);

    string result;

    const int lines = 12;
    string column[lines][5] =
    {
        {"                     ", "<green>Full</green>       ", "Satiated   ", "<yellow>Thirsty</yellow>    ", "<lightred>Bloodless</lightred>"},
                                 //Full       Satiated      Thirsty         Bloodless
        {"Metabolism           ", "fast       ", "normal     ", "slow       ", "none  "},

        {"Regeneration         ", "fast       ", "normal     ", "slow       ", "none  "},

        {"Stealth boost        ", "none       ", "none       ", "minor      ", "major "},

        {"Hunger costs         ", "full       ", "full       ", "halved     ", "none  "},

        {"\n<w>Resistances</w>\n"
         "Poison resistance    ", "           ", "           ", "+          ", "immune"},

        {"Cold resistance      ", "           ", "           ", "+          ", "++    "},

        {"Negative resistance  ", "           ", " +         ", "++         ", "+++   "},

        {"Rotting resistance   ", "           ", "           ", "+          ", "+     "},

        {"Torment resistance   ", "           ", "           ", "           ", "+     "},

        {"\n<w>Transformations</w>\n"
         "Bat form             ", "no         ", "yes        ", "yes        ", "yes   "},

        {"Other forms and \n"
         "berserk              ", "yes        ", "yes        ", "no         ", "no    "}
    };

    int current = _vampire_bloodlessness();

    for (int y = 0; y < lines; y++)  // lines   (properties)
    {
        for (int x = 0; x < 5; x++)  // columns (hunger states)
        {
            if (y > 0 && x == current)
                result += "<w>";
            result += column[y][x];
            if (y > 0 && x == current)
                result += "</w>";
        }
        result += "\n";
    }

    trim_string_right(result);
    return result;
}

void display_mutations()
{
    string mutation_s = describe_mutations(true);

    string extra = "";
    if (_num_part_suppressed)
        extra += "<brown>()</brown>  : Partially suppressed.\n";
    if (_num_full_suppressed)
        extra += "<darkgrey>(())</darkgrey>: Completely suppressed.\n";
    if (_num_transient)
        extra += "<magenta>[]</magenta>   : Transient mutations.";

    if (!extra.empty())
    {
        mutation_s += "\n\n\n\n";
        mutation_s += extra;
    }
    trim_string_right(mutation_s);

    auto vbox = make_shared<Box>(Widget::VERT);
    vbox->set_cross_alignment(Widget::STRETCH);

    const char *title_text = "Innate Abilities, Weirdness & Mutations";
    auto title = make_shared<Text>(formatted_string(title_text, WHITE));
    auto title_hbox = make_shared<Box>(Widget::HORZ);
    title_hbox->add_child(move(title));
    title_hbox->set_main_alignment(Widget::CENTER);
    vbox->add_child(move(title_hbox));

    auto switcher = make_shared<Switcher>();

    const string vamp_s = you.species == SP_VAMPIRE ?_display_vampire_attributes() : "N/A";
    const string descs[3] =  { mutation_s, vamp_s };
    for (int i = 0; i < 2; i++)
    {
        auto scroller = make_shared<Scroller>();
        auto text = make_shared<Text>(formatted_string::parse_string(
                descs[static_cast<int>(i)]));
        text->set_wrap_text(true);
        scroller->set_child(text);
        switcher->add_child(move(scroller));
    }

    switcher->current() = 0;
    switcher->set_margin_for_sdl(20, 0, 0, 0);
    switcher->set_margin_for_crt(1, 0, 0, 0);
    switcher->expand_h = false;
    switcher->align_x = Widget::STRETCH;
#ifdef USE_TILE_LOCAL
    switcher->max_size().width = tiles.get_crt_font()->char_width()*80;
#endif
    vbox->add_child(switcher);

    auto bottom = make_shared<Text>(_vampire_Ascreen_footer(true));
    bottom->set_margin_for_sdl(20, 0, 0, 0);
    bottom->set_margin_for_crt(1, 0, 0, 0);
    if (you.species == SP_VAMPIRE)
        vbox->add_child(bottom);

    auto popup = make_shared<ui::Popup>(vbox);

    bool done = false;
    int lastch;
    popup->on_keydown_event([&](const KeyEvent& ev) {
        lastch = ev.key();
        if (you.species == SP_VAMPIRE && (lastch == '!' || lastch == CK_MOUSE_CMD || lastch == '^'))
        {
            int c = 1 - switcher->current();
            switcher->current() = c;
#ifdef USE_TILE_WEB
            tiles.json_open_object();
            tiles.json_write_int("pane", c);
            tiles.ui_state_change("mutations", 0);
#endif
            bottom->set_text(_vampire_Ascreen_footer(c));
        } else
            done = !switcher->current_widget()->on_event(ev);
        return true;
    });

#ifdef USE_TILE_WEB
    tiles.json_open_object();
    tiles.json_write_string("mutations", mutation_s);
    if (you.species == SP_VAMPIRE)
        tiles.json_write_int("vampire", _vampire_bloodlessness());
    tiles.push_ui_layout("mutations", 1);
#endif

    ui::run_layout(move(popup), done);

#ifdef USE_TILE_WEB
    tiles.pop_ui_layout();
#endif
}

static int _calc_mutation_amusement_value(mutation_type which_mutation)
{
    int amusement = 12 * (11 - _get_mutation_def(which_mutation).weight);

    if (MUT_GOOD(mut_data[which_mutation]))
        amusement /= 2;
    else if (MUT_BAD(mut_data[which_mutation]))
        amusement *= 2;
    // currently is only ever one of these, but maybe that'll change?

    return amusement;
}

static bool _accept_mutation(mutation_type mutat, bool ignore_weight, bool temp)
{
    if (!is_valid_mutation(mutat))
        return false;

    if (temp && mutat == MUT_STATS)
        return false;

    // Reduce likelihood of losing body armour.
    if (mutat == MUT_DEFORMED && you.get_mutation_level(MUT_DEFORMED) && !one_chance_in(4))
        return false;

    if (temp && you.suppressed_mutation[mutat])
        return false;

    if (physiology_mutation_conflict(mutat))
        return false;

    const mutation_def& mdef = _get_mutation_def(mutat);

    if (you.get_base_mutation_level(mutat) >= mdef.levels)
        return false;

    if (you.get_base_mutation_level(mutat, true, temp, !temp) > you.get_base_mutation_level(mutat, true, temp, !temp))
        return false;

    if (ignore_weight)
        return true;

    // bias towards adding (non-innate) levels to existing innate mutations.
    const int weight = mdef.weight + you.get_innate_mutation_level(mutat);

    // Low weight means unlikely to choose it.
    return x_chance_in_y(weight, 10);
}

static mutation_type _get_mut_with_use(mutflag mt, bool temp = false)
{
    const int suppress = (mt == mutflag::bad) ? _suppress_weight(temp) : 0;
    const int tweight = lookup(total_weight, mt, 0) + suppress;
    ASSERT(tweight);

    int cweight = random2(tweight);
    for (const mutation_def &mutdef : mut_data)
    {
        if (!_mut_has_use(mutdef, mt))
            continue;

        cweight -= _mut_weight(mutdef, mt, temp);
        if (cweight >= 0)
            continue;

        return mutdef.mutation;
    }

    die("Error while selecting mutations");
}

static mutation_type _get_random_slime_mutation()
{
    return _get_mut_with_use(mutflag::jiyva);
}

static mutation_type _delete_random_slime_mutation()
{
    mutation_type mutat;

    while (true)
    {
        mutat = _get_random_slime_mutation();

        if (you.get_base_mutation_level(mutat) > 0)
            break;

        if (one_chance_in(500))
        {
            mutat = NUM_MUTATIONS;
            break;
        }
    }

    return mutat;
}

bool is_slime_mutation(mutation_type mut)
{
    if (you.species == SP_OOZOMORPH
        && (mut == MUT_CYTOPLASMIC_SUSPENSION || mut == MUT_AMORPHOUS_BODY)
        || you.species == SP_MOLTEN_GARGOYLE && mut == MUT_CORE_MELDING)
    {
        return false;
    }

    return _mut_has_use(mut_data[mut_index[mut]], mutflag::jiyva);
}

static mutation_type _get_random_xom_mutation(bool temp)
{
    mutation_type mutat = NUM_MUTATIONS;

    do
    {
        mutat = static_cast<mutation_type>(random2(NUM_MUTATIONS));

        if (one_chance_in(1000))
            return NUM_MUTATIONS;
        else if (one_chance_in(5))
            mutat = _get_mut_with_use(mutflag::xom);
    }
    while (!_accept_mutation(mutat, false, temp));

    return mutat;
}

static mutation_type _get_random_qazlal_mutation()
{
    return _get_mut_with_use(mutflag::qazlal);
}

static mutation_type _get_random_mutation(mutation_type mutclass, bool temp)
{
    mutflag mt;
    switch (mutclass)
    {
        case RANDOM_MUTATION:
            // maintain an arbitrary ratio of good to bad muts to allow easier
            // weight changes within categories - 60% good seems to be about
            // where things are right now
            mt = x_chance_in_y(3, 5) ? mutflag::good : mutflag::bad;
            break;
        case RANDOM_BAD_MUTATION:
        case RANDOM_CORRUPT_MUTATION:
            mt = mutflag::bad;
            break;
        case RANDOM_GOOD_MUTATION:
            mt = mutflag::good;
            break;
        default:
            die("invalid mutation class: %d", mutclass);
    }

    for (int attempt = 0; attempt < 1000; ++attempt)
    {
        mutation_type mut = _get_mut_with_use(mt);
        if (_accept_mutation(mut, true, temp))
            return mut;
    }

    return NUM_MUTATIONS;
}

/**
 * Does the player have a mutation that conflicts with the given mutation?
 *
 * @param mut           A mutation. (E.g. MUT_INHIBITED_REGENERATION, ...)
 * @param innate_only   Whether to only check innate mutations (from e.g. race)
 * @return              The level of the conflicting mutation.
 *                      E.g., if MUT_INHIBITED_REGENERATION is passed in and the
 *                      player has 2 levels of MUT_REGENERATION, 2 will be
 *                      returned.
 *
 *                      No guarantee is offered on ordering if there are
 *                      multiple conflicting mutations with different levels.
 */
int mut_check_conflict(mutation_type mut, bool innate_only)
{
    for (const int (&confl)[3] : conflict)
    {
        if (confl[0] != mut && confl[1] != mut)
            continue;

        const mutation_type confl_mut
           = static_cast<mutation_type>(confl[0] == mut ? confl[1] : confl[0]);

        const int level = you.get_base_mutation_level(confl_mut, true, !innate_only, !innate_only);
        if (level)
            return level;
    }

    return 0;
}

// Tries to give you the mutation by deleting a conflicting
// one, or clears out conflicting mutations if we should give
// you the mutation anyway.
// Return:
//  1 if we should stop processing (success);
//  0 if we should continue processing;
// -1 if we should stop processing (failure).
static int _handle_conflicting_mutations(mutation_type mutation,
                                         bool override,
                                         const string &reason,
                                         bool temp = false,
                                         bool innate = false)
{
    // If we have one of the pair, delete all levels of the other,
    // and continue processing.
    for (const int (&confl)[3] : conflict)
    {
        for (int j = 0; j <= 1; ++j)
        {
            const mutation_type a = (mutation_type)confl[j];
            const mutation_type b = (mutation_type)confl[1-j];

            if (mutation == a && you.get_base_mutation_level(b) > 0)
            {
                if (innate && you.has_innate_mutation(b))
                {
                    // Override for conflicting innate mutations.
                    _delete_single_mutation_level(b, reason, false, true);
                    return -1;
                }

                // can never delete innate mutations. For case -1 and 0, fail if there are any, otherwise,
                // make sure there is a non-innate instance to delete.
                if (you.has_innate_mutation(b) &&
                    (confl[2] != 1
                     || you.get_base_mutation_level(b, true, false, false) == you.get_base_mutation_level(b)))
                {
                    dprf("Delete mutation failed: have innate mutation %d at level %d, you.mutation at level %d", b,
                        you.get_innate_mutation_level(b), you.get_base_mutation_level(b));
                    return -1;
                }

                // at least one level of this mutation is temporary
                const bool temp_b = you.has_temporary_mutation(b);

                // confl[2] indicates how the mutation resolution should proceed (see `conflict` a the beginning of this file):
                switch (confl[2])
                {
                case -1:
                    // Fail if not forced, otherwise override.
                    if (!override)
                        return -1;
                case 0:
                    // Ignore if not forced, otherwise override.
                    // All cases but regen:slowmeta will currently trade off.
                    if (override)
                    {
                        while (_delete_single_mutation_level(b, reason, true))
                            ;
                    }
                    break;
                case 1:
                    // If we have one of the pair, delete a level of the
                    // other, and that's it.
                    //
                    // Temporary mutations can co-exist with things they would
                    // ordinarily conflict with. But if both a and b are temporary,
                    // mark b for deletion.
                    if ((temp || temp_b) && !(temp && temp_b))
                        return 0;       // Allow conflicting transient mutations
                    else
                    {
                        _delete_single_mutation_level(b, reason, true);
                        return 1;     // Nothing more to do.
                    }

                default:
                    die("bad mutation conflict resolution");
                }
            }
        }
    }

    return 0;
}

static int _body_covered()
{
    // Check how much of your body is covered by scales, etc.
    // Note: this won't take into account forms, so is only usable for checking in general.
    int covered = 0;

    if (you.species == SP_NAGA || you.char_class == JOB_NAGA)
        covered++;

    if (species_is_draconian(you.species))
        covered += 3;

    for (mutation_type scale : _all_scales)
        covered += you.get_base_mutation_level(scale) * 3;

    return covered;
}

mutation_type get_scales()
{
    for (mutation_type scale : _all_scales)
    {
        if (you.get_base_mutation_level(scale))
            return scale;
    }

    return MUT_NON_MUTATION;
}

bool physiology_mutation_conflict(mutation_type mutat, bool ds_roll)
{
    // If demonspawn, and mutat is a scale, see if they were going
    // to get it sometime in the future anyway; otherwise, conflict.
    if ((you.species == SP_DEMONSPAWN || you.char_class == JOB_DEMONSPAWN)
        && !ds_roll && _is_covering(mutat)
        && find(_all_scales, _all_scales+ARRAYSZ(_all_scales), mutat) !=
                _all_scales+ARRAYSZ(_all_scales))
    {
        return none_of(begin(you.demonic_traits), end(you.demonic_traits),
                       [=](const player::demon_trait &t) {
                           return t.mutation == mutat;});
    }

    if (you.undead_or_demonic())
    {
        if (mutat == MUT_BERSERK || mutat == MUT_POISON_RESISTANCE)
            return true; // Poison Resist is redundant and undeads can't berserk.
    }

    // Don't give felids or trolls claws, etc.
    if (ds_roll && you.innate_mutation[mutat] > 0)
        return true;

    if (you.species == SP_FAIRY && (mutat == MUT_CLAWS || mutat == MUT_HORNS))
        return true;

    // Strict 3-scale limit.
    if (_is_covering(mutat) && _body_covered() >= 3 && !ds_roll)
        return true;

    // Only Nagas and Draconians can get this one.
    if (you.species != SP_NAGA && !species_is_draconian(you.species)
        && you.char_class != JOB_NAGA && mutat == MUT_STINGER)
    {
        return true;
    }

    // Need tentacles to grow something on them.
    if (you.species != SP_OCTOPODE && mutat == MUT_TENTACLE_SPIKE)
        return true;

    if (mutat == MUT_STRONG_NOSE && !you.can_smell())
        return true;

    if ((mutat == MUT_FAIRY_LIGHT || mutat == MUT_SILENCE_AURA) && !you.innate_mutation[mutat])
        return true;

    if (mutat == MUT_DETERIORATION && you.undead_state())
        return true;

    // Too squishy for horns.
    if (you.species == SP_OCTOPODE && mutat == MUT_HORNS)
        return true;

    // No feet.
    if (!player_has_feet(false, false)
        && (mutat == MUT_HOOVES || mutat == MUT_TALONS || mutat == MUT_FROG_LEGS))
    {
        return true;
    }

    // Felids have innate claws, and unlike trolls/ghouls, there are no
    // increases for them.
    if (you.species == SP_FELID  && mutat == MUT_CLAWS)
        return true;

    // Merfolk have no feet in the natural form, and we never allow mutations
    // that show up only in a certain transformation.
    if ((you.species == SP_MERFOLK || you.char_class == JOB_MERFOLK)
        && !ds_roll && (mutat == MUT_TALONS || mutat == MUT_HOOVES || mutat == MUT_FROG_LEGS))
    {
        return true;
    }

    if (you.has_innate_mutation(MUT_STASIS))
    {
        // Formicids have stasis and so prevent mutations that would do nothing.
        if (mutat == MUT_BERSERK
            || mutat == MUT_BLINK
            || mutat == MUT_TELEPORT)
        {
            return true;
        }
    }

    // Already immune.
    if ((player_res_poison(false, false, false) == 3) && mutat == MUT_POISON_RESISTANCE)
        return true;

    // We can't use is_useless_skill() here, since species that can still wear
    // body armour can sacrifice armour skill with Ru.
    if (species_apt(SK_ARMOUR) == UNUSABLE_SKILL
        && (mutat == MUT_DEFORMED || mutat == MUT_STURDY_FRAME))
    {
        return true;
    }

    equipment_type eq_type = EQ_NONE;

    // Mutations of the same slot conflict
    if (is_body_facet(mutat))
    {
        // Find equipment slot of attempted mutation
        for (const body_facet_def &facet : _body_facets)
            if (mutat == facet.mut)
                eq_type = facet.eq;

        if (eq_type != EQ_NONE)
        {
            for (const body_facet_def &facet : _body_facets)
            {
                if (eq_type == facet.eq
                    && mutat != facet.mut
                    && you.get_base_mutation_level(facet.mut))
                {
                    return true;
                }
            }
        }
    }

    return false;
}

static void _stat_mut_msg(string &result, bool gain, int s, int i, int d, bool terse)
{
    ostringstream ostr, STR, INT, DEX;

    if (s)
    {
        ostr << "STR " << ((s > 0) ? "+" : "") << s;
        STR << (abs(s) > 4 ? "extremely " : abs(s) > 2 ? "very " : "");
        STR << stat_desc(STAT_STR, s > 0 ? SD_INCREASE : SD_DECREASE);
    }

    if (i)
    {
        if (s)
            ostr << ", ";
        ostr << "INT " << ((i > 0) ? "+" : "") << i;
        INT << (abs(i) > 4 ? "extremely " : abs(i) > 2 ? "very " : "");
        INT << stat_desc(STAT_INT, i > 0 ? SD_INCREASE : SD_DECREASE);
    }

    if (d)
    {
        if (s || i)
            ostr << ", ";
        ostr << "DEX " << ((d > 0) ? "+" : "") << d;
        DEX << (abs(d) > 4 ? "extremely " : abs(d) > 2 ? "very " : "");
        DEX << stat_desc(STAT_DEX, d > 0 ? SD_INCREASE : SD_DECREASE);
    }

    if (s || i || d)
    {
        if (terse)
        {
            result = ostr.str();
            return;
        }

        ostringstream retval;
        retval << "You " << (gain ? "feel " : "are ") << STR.str();
        retval << (s && i ? (d ? ", " : " and ") : "") << INT.str();
        retval << ((s || i) && d ? " and " : "") << DEX.str();
        retval << ". (" << ostr.str() << ")";

        result = retval.str();
        return;
    }

    if (gain)
    {
        result = "But nothing happened . . .";
        return;
    }

    result = "stats";
    return;
}

// Bookkeeps that stat mut's actual level and displays message.
static void _stat_mut_gain(bool gain, int orig_STR, int orig_INT, int orig_DEX)
{
    const bool end = !(you.mutated_stats[STAT_STR] || you.mutated_stats[STAT_INT] || you.mutated_stats[STAT_DEX]);

    int s = you.mutated_stats[STAT_STR] - orig_STR;
    int i = you.mutated_stats[STAT_INT] - orig_INT;
    int d = you.mutated_stats[STAT_DEX] - orig_DEX;

    string MSG = "";
    _stat_mut_msg(MSG, true, s, i, d, false);

    mprf(MSGCH_MUTATION, "%s %s", gain ? "You feel your attributes changing!" : 
                                   end ? "Your attributes return to normal." : 
                                         "Your attributes move closer to where they started.", MSG.c_str());

    you.mutation[MUT_STATS] = min(128, div_round_up(abs(you.mutated_stats[STAT_STR]) + abs(you.mutated_stats[STAT_INT]) + abs(you.mutated_stats[STAT_DEX]), 10));
}

static void _unmutate_stats()
{
    int x = 9 + random2(2);

    int orig_STR = you.mutated_stats[STAT_STR];
    int orig_INT = you.mutated_stats[STAT_INT];
    int orig_DEX = you.mutated_stats[STAT_DEX];

    for (; x > 0; x--)
    {
        int y = random2(3);
        switch (y)
        {
        case 0:
            if (you.mutated_stats[STAT_STR] > 0) { you.mutated_stats[STAT_STR]--; break; }
            if (you.mutated_stats[STAT_STR] < 0) { you.mutated_stats[STAT_STR]++; break; } // else fallthrough;
        case 1:
            if (you.mutated_stats[STAT_INT] > 0) { you.mutated_stats[STAT_INT]--; break; }
            if (you.mutated_stats[STAT_INT] < 0) { you.mutated_stats[STAT_INT]++; break; } // else fallthrough;
        case 2:
            if (you.mutated_stats[STAT_DEX] > 0) { you.mutated_stats[STAT_DEX]--; break; }
            if (you.mutated_stats[STAT_DEX] < 0) { you.mutated_stats[STAT_DEX]++; break; } // else continue . . .
            if (you.mutated_stats[STAT_STR] > 0) { you.mutated_stats[STAT_STR]--; break; }
            if (you.mutated_stats[STAT_STR] < 0) { you.mutated_stats[STAT_STR]++; break; }
            if (you.mutated_stats[STAT_INT] > 0) { you.mutated_stats[STAT_INT]--; break; }
            if (you.mutated_stats[STAT_INT] < 0) { you.mutated_stats[STAT_INT]++; break; }
            x -= 20; break;
        }
    }

    _stat_mut_gain(false, orig_STR, orig_INT, orig_DEX);
}

static void _stat_potion(mutation_type type)
{
    const int gain = 3 + random2(4);
    const int orig_STR = you.mutated_stats[STAT_STR];
    const int orig_INT = you.mutated_stats[STAT_INT];
    const int orig_DEX = you.mutated_stats[STAT_DEX];
    const stat_type stat = type == MUT_STRONG ? STAT_STR :
                           type == MUT_AGILE  ? STAT_DEX
                                              : STAT_INT;

    you.mutated_stats[stat] += gain;

    _stat_mut_gain(true, orig_STR, orig_INT, orig_DEX);
}

static void _mutate_stats(mutation_type type)
{
    int positive = 0;
    int negative = 0;

    if (type == RANDOM_GOOD_MUTATION)
    {
        int x = random2(4);
        positive = 4 + x;
    }
    else if (type == RANDOM_BAD_MUTATION)
    {
        int x = random2(4);
        negative = 4 + x;
    }

    int z = 10;
    while (positive + negative < z)
    {
        if (coinflip())
            positive++;
        else
            negative++;
    }

    int orig_STR = you.mutated_stats[STAT_STR];
    int orig_INT = you.mutated_stats[STAT_INT];
    int orig_DEX = you.mutated_stats[STAT_DEX];

    bool str_changed = false;
    bool int_changed = false;
    bool dex_changed = false;

    if (positive)
    {
        if (coinflip())
        {
            int x = random2(3);
            switch (x)
            {
            case 0: you.mutated_stats[STAT_STR] += positive; str_changed = true; break;
            case 1: you.mutated_stats[STAT_INT] += positive; int_changed = true; break;
            case 2: you.mutated_stats[STAT_DEX] += positive; dex_changed = true; break;
            }
        }
        else
        {
            int x = random2(3);
            switch (x)
            {
            case 0: you.mutated_stats[STAT_STR] += positive / 2; str_changed = true; break;
            case 1: you.mutated_stats[STAT_INT] += positive / 2; int_changed = true; break;
            case 2: you.mutated_stats[STAT_DEX] += positive / 2; dex_changed = true; break;
            }

            x = random2(3);
            switch (x)
            {
            case 0: if (!str_changed) { you.mutated_stats[STAT_STR] += div_round_up(positive, 2); str_changed = true; break; } // else fallthrough;
            case 1: if (!int_changed) { you.mutated_stats[STAT_INT] += div_round_up(positive, 2); int_changed = true; break; } // else fallthrough;
            case 2: if (!dex_changed) { you.mutated_stats[STAT_DEX] += div_round_up(positive, 2); dex_changed = true; break; } // and repeat if we got here.
                    if (coinflip())   { you.mutated_stats[STAT_STR] += div_round_up(positive, 2); str_changed = true; break; }
                                        you.mutated_stats[STAT_INT] += div_round_up(positive, 2); int_changed = true; break;
            }
        }
    }

    bool success = false;
    if (coinflip())
    {
        int x = random2(3);
        switch (x)
        {
        case 0: if ((you.max_stat(STAT_STR, true) >= 1 + negative) && !str_changed) { you.mutated_stats[STAT_STR] -= negative; success = true; break; } // else fallthrough;
        case 1: if ((you.max_stat(STAT_INT, true) >= 1 + negative) && !int_changed) { you.mutated_stats[STAT_INT] -= negative; success = true; break; } // else fallthrough;
        case 2: if ((you.max_stat(STAT_DEX, true) >= 1 + negative) && !dex_changed) { you.mutated_stats[STAT_DEX] -= negative; success = true; break; } // and repeat if we got here.
                if ((you.max_stat(STAT_STR, true) >= 1 + negative) && !str_changed) { you.mutated_stats[STAT_STR] -= negative; success = true; break; }
                if ((you.max_stat(STAT_INT, true) >= 1 + negative) && !int_changed) { you.mutated_stats[STAT_INT] -= negative; success = true; break; }
        }
    }

    if (!success)
    {
        for (int i = negative; i > 0; --i)
        {
            int x = random2(3);
            switch (x)
            {
            case 0: if (you.max_stat(STAT_STR, true) >= 2 && !str_changed) { you.mutated_stats[STAT_STR]--;  break; } // else fallthrough;
            case 1: if (you.max_stat(STAT_INT, true) >= 2 && !int_changed) { you.mutated_stats[STAT_INT]--;  break; } // else fallthrough;
            case 2: if (you.max_stat(STAT_DEX, true) >= 2 && !dex_changed) { you.mutated_stats[STAT_DEX]--;  break; } // and repeat if we got here.
                    if (you.max_stat(STAT_STR, true) >= 2 && !str_changed) { you.mutated_stats[STAT_STR]--;  break; } // . . .
                    if (you.max_stat(STAT_INT, true) >= 2 && !int_changed) { you.mutated_stats[STAT_INT]--;  break; } // . . .
            }
        }
    }

    _stat_mut_gain(true, orig_STR, orig_INT, orig_DEX);
}

/**
 * Do a resistance check for the given mutation permanence class.
 * Does not include divine intervention!
 *
 * @param mutclass The type of mutation that is checking resistance
 * @param beneficial Is the mutation beneficial?
 *
 * @return True if a mutation is successfully resisted, false otherwise.
**/
static bool _resist_mutation(mutation_permanence_class mutclass,
                             bool beneficial)
{
    if (you.get_mutation_level(MUT_MUTATION_RESISTANCE) == 3)
        return true;

    bool you_res = you.get_mutation_level(MUT_MUTATION_RESISTANCE);

    // draconian scales:
    if (!you_res && (you.get_mutation_level(MUT_DRACONIAN_DEFENSE, true)))
    {
        if (you.drac_colour == DR_PLATINUM || you.drac_colour == DR_OLIVE || you.drac_colour == DR_SILVER)
            you_res = true;
        if (you.drac_colour == DR_SCINTILLATING && one_chance_in(3))
            you_res = true;
    }

    const int mut_resist_chance = mutclass == MUTCLASS_TEMPORARY ? 2 : 3;
    if (you_res && !one_chance_in(mut_resist_chance))
        return true;

    // To be nice, beneficial mutations go through removable sources of rMut.
    if (you.rmut_from_item() && !beneficial
        && !one_chance_in(mut_resist_chance))
    {
        return true;
    }

    return false;
}

/*
 * Does the player rot instead of mutating?
 * Right now this is coextensive with whether the player is unable to mutate.
 * For most undead, they will never mutate and always rot instead; vampires always mutate and never rot.
 *
 * @return true if so.
 */
bool undead_mutation_rot(bool god_gift)
{
    return !god_gift && !you.can_safely_mutate();
}

static string _pseudopod_message(bool gain)
{
    string brand = "Buggy";
    string effect = "";
    switch (you.pseudopod_brand)
    {
    case SPWPN_MOLTEN:
        brand = "Scorching";
        effect = "dousing the target in sticky flames.";
        break;
    case SPWPN_ACID:
        brand = "Corrosive";
        break;
    case SPWPN_ELECTROCUTION:
        brand = "Electrifying";
        effect = "malmutating the target.";
        break;
    case SPWPN_VENOM:
        brand = "Noxious";
        effect = "slowing targets, who aren't immune to poison.";
        break;
    case SPWPN_VAMPIRISM:
        brand = "Vampiric";
        if (!you_foodless(false))
            effect = "feeding you.";
        break;
    default:
        break;
    }

    ostringstream ostr;
    const mutation_def& mdef = _get_mutation_def(MUT_PSEUDOPODS);
    // Assumes no species has a unique message for this.
    ostr << brand.c_str();
    if (gain)
        ostr << mdef.gain[0];
    else
        ostr << mdef.have[0];
    if (effect.length() && !gain)
        ostr << ", sometimes " << effect.c_str();
    else
        ostr << ".";
    return ostr.str();
}

static species_mutation_message _spmut_msg(mutation_type mutat)
{
    for (unsigned int i = 0; i < spmu_length; ++i)
    {
        if (spmu_data[i].species == you.species)
        {
            if (spmu_data[i].mutation == mutat)
                return spmu_data[i];
        }
    }
    return spmu_data[0];
}

static string _drac_def_msg()
{
    ostringstream ostr;
    ostr << "Your " << scale_type() << " scales ";
    switch (you.drac_colour)
    {
    case DR_BLACK:
        ostr << "make you much more stealthy. (Stealth++)";
        break;
    case DR_BLOOD:
        ostr << "grant partial resistance to hellfire and unholy torment. (rTorm, rHellfire)";
        break;
    case DR_BLUE:
        ostr << "grant you resistance to electric shocks. (rElec)";
        break;
    case DR_BONE: // Special case; no scales.
        return "Your tough skeletal form vastly boosts your defenses. (AC+++)\nHowever; you are also weak to shatter and Lee's Rapid Deconstruction.";
    default:
    case DR_BROWN:
        ostr << "don't do anything special. (This message shouldn't ever display.)";
        break;
    case DR_CYAN:
        ostr << "grant you immunity to the effects of clouds and air. (rCloud, rAir)";
        break;
    case DR_GOLDEN:
        ostr << "grant you resistance to fire, cold and poison. (rF+, rC+, rPois)";
        break;
    case DR_GREEN:
        ostr << "grant you resistance to poison. (rPois)";
        break;
    case DR_LIME:
        ostr << "grant you resistance to acid and corrosion. (rCorr)";
        break;
    case DR_MAGENTA:
        ostr << "passively repel missiles. (rMsl)";
        break;
    case DR_OLIVE:
        ostr << "grant you resistance to rotting caused by mutagenic radiation. (rMut)";
        break;
    case DR_PEARL:
        if (you.char_class == JOB_DEMONSPAWN)
            ostr << "boost your defenses, increase your resistance to negative energy and remove your vulnerability to holy energy. (AC++, rN+, rHoly)";
        else
            ostr << "boost your defenses and vastly increase your resistance to negative energy. (AC++, rN+++)";
        break;
    case DR_PINK:
        ostr << "grant you exceptional clarity of mind. (clarity)";
        break;
    case DR_PLATINUM:
        ostr << "increase your movement speed and make you resistant to mutation. (fast, rMut)";
        break;
    case DR_PURPLE:
        ostr << "protect you from hostile magic. (MR++)";
        break;
    case DR_RED:
        ostr << "grant you resistance to heat and fire. (rF+)";
        break;
    case DR_SCINTILLATING:
        ostr << "chaotically grant elemental resistances and boost your physical defenses a bit. (AC+, Chaos+)";
        break;
    case DR_SILVER:
        ostr << "boost your physical defenses and make you resistant to mutation. (AC++, rMut)";
        break;
    case DR_TEAL:
        ostr << "hold together your spectral form.";
        break;
    case DR_WHITE:
        ostr << "grant you resistance to cold and ice. (rC+)";
        break;
    }
    return ostr.str();
}

// type 1 = gain; 0 = have; -1 = lose.
static string _drac_enhancer_msg(int type)
{
    ostringstream ostr;
    if (type > 0)
        ostr << _get_mutation_def(MUT_DRACONIAN_ENHANCER).gain[0];
    else if (type < 0)
        ostr << _get_mutation_def(MUT_DRACONIAN_ENHANCER).lose[you.get_mutation_level(MUT_DRACONIAN_ENHANCER, false)];
    else
        ostr << _get_mutation_def(MUT_DRACONIAN_ENHANCER).have[you.get_mutation_level(MUT_DRACONIAN_ENHANCER, false) - 1];

    switch (you.drac_colour)
    {
    case DR_BLACK:      ostr << "death.";                                       break;
    case DR_BLOOD:      ostr << "death, curses and the sky.";                   break;
    case DR_BLUE:       ostr << "the sky.";                                     break;
    case DR_BONE:       ostr << "charms and the earth.";                        break;
    case DR_CYAN:       ostr << "translocation.";                               break;
    case DR_GOLDEN:     ostr << "fire, ice and venom.";                         break;
    case DR_GREEN:      ostr << "venom.";                                       break;
    case DR_LIME:       ostr << "transmutation.";                               break;
    case DR_MAGENTA:    ostr << "charms.";                                      break;
    case DR_OLIVE:      ostr << "venom and the sky.";                           break;
    case DR_PEARL:      ostr << "charms, summonings and the earth.";            break;
    case DR_PINK:       ostr << "summonings.";                                  break;
    case DR_PLATINUM:   ostr << "translocation, transmutation and hexes.";      break;
    case DR_PURPLE:     ostr << "hexes.";                                       break;
    case DR_RED:        ostr << "fire.";                                        break;
    case DR_SILVER:     ostr << "the earth.";                                   break;
    case DR_TEAL:       ostr << "translocation and transmutation.";             break;
    case DR_WHITE:      ostr << "ice.";                                         break;
    default:    // Shouldn't display ever; hopefully.
    case DR_BROWN:      ostr << "bugginess.";                                   break;
    case DR_SCINTILLATING:  // Special case; messaging unlike the others.
        if (type > 0)
        {
            if (you.get_mutation_level(MUT_DRACONIAN_ENHANCER) == 1)
                return "Your magic feels more chaotic and you randomly feel bursts of power when casting.";
            else
                return "The bursts of power when casting become more profound.";
        }
        else if (type < 0)
        {
            if (you.get_mutation_level(MUT_DRACONIAN_ENHANCER) == 0)
                return "Your magic no longer feels chaotic.";
            else
                return "The bursts of power while casting are less pronounced.";
        }
        else
        {
            if (you.get_mutation_level(MUT_DRACONIAN_ENHANCER) > 1)
                return "Your magic is chaotic and you often have strongly boosted spellpower when casting.";
            else
                return "Your magic is chaotic and you often have boosted spellpower when casting.";
        }
    }
    return ostr.str();
}

static void _skill_rescale()
{
    uint8_t saved_skills[NUM_SKILLS];
    for (skill_type sk = SK_FIRST_SKILL; sk < NUM_SKILLS; ++sk)
    {
        saved_skills[sk] = you.skills[sk];
        check_skill_level_change(sk, false);
    }

    // Produce messages about skill increases/decreases. We
    // restore one skill level at a time so that at most the
    // skill being checked is at the wrong level.
    for (skill_type sk = SK_FIRST_SKILL; sk < NUM_SKILLS; ++sk)
    {
        you.skills[sk] = saved_skills[sk];
        check_skill_level_change(sk);
    }

    redraw_screen();
}

static bool _is_suppressable_mutation(mutation_type mut)
{
    switch (mut)
    {
    // If mummy becomes semi-living it's +Int shouldn't be killed.
    case MUT_ANCIENT_WISDOM:
    // Not making a fairy solid
    case MUT_INSUBSTANTIAL:
    // Not getting rid of the racial gimmick
    case MUT_MERFOLK_TAIL:
    // Feels extremely weak and more lie a natural fact
    case MUT_COLD_BLOODED:
    // Weird flavourwise to suppress
    case MUT_GODS_PITY:
    // Let's not remeld Jivya's special slots
    case MUT_ARM_MORPH:
    case MUT_CORE_MELDING:
    case MUT_CYTOPLASMIC_SUSPENSION:
    case MUT_GELATINOUS_TAIL:
    case MUT_AMORPHOUS_BODY:
    case MUT_TENDRILS:
    // Silencing a Silent Spectre would be too brutal.
    case MUT_SILENT_CAST:
    // Complicated to suppress and not worth the trouble.
    case MUT_STATS:
    // Uhh . . . yea Suppressing itself makes no sense.
    case MUT_SUPPRESSION:
    // Too fundamental to Draconian Race to Allow.
    case MUT_DRACONIAN_DEFENSE:
    case MUT_MAJOR_MARTIAL_APT_BOOST:
    case MUT_MINOR_MARTIAL_APT_BOOST:
    case MUT_DEFENSIVE_APT_BOOST:
    // Felids mutating . . . feet? Nah
    case MUT_PAWS:
    // Losing the ability to use your weapon combo would be too harsh.
    case MUT_MULTIARM:
    // no returning to life/death
    case MUT_HALF_DEATH:
        return false;
    // no feet on centaurs.
    case MUT_HOOVES:
        if (you.species == SP_CENTAUR || you.char_class == JOB_CENTAUR)
            return false;
        return true;
    // no gaining armour.
    case MUT_DEFORMED:
        if (you.innate_mutation[MUT_DEFORMED] > 1)
            return false;
    default:
        break;
    }
    // Ru exceptions are handled by a specific sacrifice check.
    return true;
}

static void _post_gain_effects(mutation_type mutat)
{
    const int cur_base_level = you.get_base_mutation_level(mutat);

    // Do post-mutation effects.
    switch (mutat)
    {
    case MUT_UNSKILLED:
        _skill_rescale();
        break;

    case MUT_FRAIL:
    case MUT_ROBUST:
    case MUT_RUGGED_BROWN_SCALES:
    case MUT_ACID_WAVE:
        calc_hp();
        break;

    case MUT_LOW_MAGIC:
    case MUT_HIGH_MAGIC:
        calc_mp();
        break;

    case MUT_PASSIVE_MAPPING:
        add_daction(DACT_REAUTOMAP);
        break;

    case MUT_FROG_LEGS:
    case MUT_HOOVES:
    case MUT_TALONS:
        // Hooves and talons force boots off.
        remove_one_equip(EQ_BOOTS, false, true);
        // Recheck Ashenzari bondage in case our available slots changed.
        ash_check_bondage();
        break;

    case MUT_DEFORMED:
        if (cur_base_level > 1 && !you.get_mutation_level(MUT_CORE_MELDING))
        {
            remove_one_equip(EQ_BODY_ARMOUR, false, true);
            // Recheck Ashenzari bondage in case our available slots changed.
            ash_check_bondage();
        }
        break;

    case MUT_CLAWS:
        // Claws force gloves off at 2.
        if (cur_base_level >= 2 && !you.melded[EQ_GLOVES])
            remove_one_equip(EQ_GLOVES, false, true);
        // Recheck Ashenzari bondage in case our available slots changed.
        ash_check_bondage();
        break;

    case MUT_ANTENNAE:
        // Antennae remove all headgear. Same algorithm as with
        // glove removal.

        if (!you.melded[EQ_HELMET])
            remove_one_equip(EQ_HELMET, false, true);
        // Intentional fall-through
    case MUT_HORNS:
    case MUT_BEAK:
        // Horns, beaks, and antennae force hard helmets off.
        if (you.equip[EQ_HELMET] != -1
            && is_hard_helmet(you.inv[you.equip[EQ_HELMET]])
            && !you.melded[EQ_HELMET])
        {
            remove_one_equip(EQ_HELMET, false, true);
        }
        // Recheck Ashenzari bondage in case our available slots changed.
        ash_check_bondage();
        break;

    case MUT_ACUTE_VISION:
        // We might have to turn autopickup back on again.
        autotoggle_autopickup(false);
        break;

    case MUT_NIGHTSTALKER:
        update_vision_range();
        break;

    case MUT_BIG_WINGS:
#ifdef USE_TILE
        init_player_doll();
#endif
        break;

    default:
        break;
    }

    xom_is_stimulated(_calc_mutation_amusement_value(mutat));
}

static bool _pregain_effects(mutation_type mutat, bool temp = false, mutation_type which_mutation = RANDOM_MUTATION)
{
    const int cur_base_level = you.get_base_mutation_level(mutat);

    bool gain_msg = true;

    // More than three messages, need to give them by hand.
    switch (mutat)
    {
    case MUT_STATS:
        _mutate_stats(which_mutation);
        gain_msg = false;
        break;

    case MUT_SUPPRESSION:
        ASSERT(suppress_mutation(temp));
        gain_msg = false;
        break;

    case MUT_AMORPHOUS_BODY:
        _transpose_gear();
        break;

    case MUT_PSEUDOPODS:
        mprf(MSGCH_MUTATION, "%s", _pseudopod_message(true).c_str());
        gain_msg = false;
        break;

    case MUT_HALF_DEATH:
        if (you.mutation[mutat] == 1)
        {
            switch (you.undead_state(false))
            {
            default:
            case US_SEMI_ALIVE:
                mprf(MSGCH_MUTATION, "You feel yourself fall into an odd twilight state between life and death.");
                break;
            case US_SEMI_UNDEAD:
                mprf(MSGCH_MUTATION, "You feel yourself come partially back to life.");
                break;
            }
            gain_msg = false;
        }
        break;

    case MUT_LARGE_BONE_PLATES:
    {
        species_mutation_message msg = _spmut_msg(mutat);
        if (msg.mutation != MUT_NON_MUTATION)
            mprf(MSGCH_MUTATION, "%s", msg.gain[cur_base_level - 1]);
        else
        {
            const mutation_def& mdef = _get_mutation_def(MUT_LARGE_BONE_PLATES);
            const char *arms;
            if (you.species == SP_FELID)
                arms = "legs";
            else if (you.species == SP_OCTOPODE)
                arms = "tentacles";
            else
                break;

            mprf(MSGCH_MUTATION, "%s",
                replace_all(mdef.gain[cur_base_level - 1], "arms",
                    arms).c_str());
        }
        gain_msg = false;
    }
    break;

    case MUT_DRACONIAN_DEFENSE:
        mprf(MSGCH_MUTATION, "%s", _drac_def_msg().c_str());
        gain_msg = false;
        break;

    case MUT_DRACONIAN_ENHANCER:
        mprf(MSGCH_MUTATION, "%s", _drac_enhancer_msg(1).c_str());
        gain_msg = false;
        break;

    case MUT_MISSING_HAND:
    {
        const mutation_def& mdef = _get_mutation_def(MUT_MISSING_HAND);
        const char *hands;
        if (you.species == SP_FELID)
            hands = "front paws";
        else if (you.species == SP_OCTOPODE)
            hands = "tentacles";
        else
            break;
        mprf(MSGCH_MUTATION, "%s",
            replace_all(mdef.gain[cur_base_level - 1], "hands",
                hands).c_str());
        gain_msg = false;
    }
    break;

    case MUT_NO_LOVE:
        if (you.attribute[ATTR_SKELETON])
        {
            you.attribute[ATTR_SKELETON] = 0;
            mprf(MSGCH_DURATION, "Skeletons will no longer rise from your steps.");
        }
        break;

    case MUT_SPIT_POISON:
        // Breathe poison replaces spit poison (so it takes the slot).
        if (cur_base_level >= 2)
            abil_swap(ABIL_SPIT_POISON, ABIL_BREATHE_POISON);
        break;

    default:
        break;
    }
    
    return gain_msg;
}

static bool _unsuppress_mutation(bool temp)
{
    vector<mutation_type> targets;

    for (int i = 0; i < NUM_MUTATIONS; ++i)
    {
        const mutation_type m = static_cast<mutation_type>(i);
        if (you.suppressed_mutation[i] == (temp ? 2 : 1))
            targets.emplace_back(m);
    }

    if (!targets.size())
        return false;

    const mutation_type target = targets[random2(targets.size())];
    const bool gain_msg = _pregain_effects(target);
    const mutation_def& mdef = _get_mutation_def(target);
    const species_mutation_message msg = _spmut_msg(target);

    you.suppressed_mutation[target] = 0;

    if (msg.mutation == MUT_NON_MUTATION)
    {
        mprf(MSGCH_MUTATION, "Your %s is no longer suppressed.", mdef.short_desc);
        if (gain_msg)
            mprf(MSGCH_MUTATION, "%s", mdef.gain[you.mutation[target] - 1]);
    }
    else
    {
        mprf(MSGCH_MUTATION, "You %s is no longer suppressed.", msg.short_desc);
        if (gain_msg)
            mprf(MSGCH_MUTATION, "%s", msg.gain[you.mutation[target] - 1]);
    }

    _post_gain_effects(target);

    return true;
}

bool suppress_mutation(bool temp, bool zin, bool check)
{
    vector<mutation_type> targets;

    for (int i = 0; i < NUM_MUTATIONS; ++i)
    {
        const mutation_type m = static_cast<mutation_type>(i);
        if (you.mutation[i] && !you.sacrifices[i] && !you.suppressed_mutation[i] && _is_suppressable_mutation(m))
        {
            if (you.temp_mutation[i])
                continue;

            if (you.innate_mutation[i])
            {
                if (!zin || _mut_has_use(mut_data[i], mutflag::jiyva))
                    targets.emplace_back(m);
            }
            else if (temp)
                targets.emplace_back(m);

            if (zin)
            {
                for (unsigned int j = 0; j < you.demonic_traits.size(); j++)
                {
                    if (you.demonic_traits[j].mutation == i &&
                        you.demonic_traits[j].level_gained <= you.experience_level)
                    {
                        targets.emplace_back(m);
                        targets.emplace_back(m);
                        j += 1000;
                    }
                }
            }
        }
    }

    if (targets.empty())
        return false;

    if (check)
        return true;

    const mutation_type target = targets[random2(targets.size())];

    you.suppressed_mutation[target] = temp ? 2 : 1;

    const bool loss_msg = _post_loss_effects(target);
    const mutation_def& mdef = _get_mutation_def(target);
    const species_mutation_message msg = _spmut_msg(target);

    if (msg.mutation == MUT_NON_MUTATION)
    {
        mprf(MSGCH_MUTATION, "You feel your %s being suppressed.", mdef.short_desc);
        if (loss_msg)
            mprf(MSGCH_MUTATION, "%s", mdef.lose[you.mutation[target] - 1]);
    }
    else
    {
        mprf(MSGCH_MUTATION, "You feel your %s being suppressed.", msg.short_desc);
        if (loss_msg)
            mprf(MSGCH_MUTATION, "%s", msg.lose[you.mutation[target] - 1]);
    }

    return true;
}

/*
 * Try to mutate the player, along with associated bookkeeping. This accepts mutation categories as well as particular mutations.
 *
 * In many cases this will produce only 1 level of mutation at a time, but it may mutate more than one level if the mutation category is corrupt or qazlal.
 *
 * If the player is at the mutation cap, this may fail.
 *   1. If mutclass is innate, this will attempt to replace temporary and normal mutations (in that order) and will fail if this isn't possible (e.g. there are only innate levels).
 *   2. Otherwise, this will fail. This means that a temporary mutation can block a permanent mutation of the same type in some circumstances.
 *
 * If the mutation conflicts with an existing one it may fail. See `_handle_conflicting_mutations`.
 *
 * If the player is undead, this may rot instead. Rotting counts as success.
 *
 * @param which_mutation    the mutation to use.
 * @param reason            the explanation for how the player got mutated.
 * @param failMsg           whether to do any messaging if this fails.
 * @param force_mutation    whether to override mutation protection and the like.
 * @param god_gift          is this a god gift? Entails overriding mutation resistance if not forced.
 * @param mutclass          is the mutation temporary, regular, or permanent (innate)? permanent entails force_mutation.
 *
 * @return whether the mutation succeeded.
 */
bool mutate(mutation_type which_mutation, const string &reason, bool failMsg,
            bool force_mutation, bool god_gift, bool beneficial,
            mutation_permanence_class mutclass)
{
    if (which_mutation == RANDOM_BAD_MUTATION
        && mutclass == MUTCLASS_NORMAL
        && crawl_state.disables[DIS_AFFLICTIONS])
    {
        return true; // no fallbacks
    }

    god_gift |= crawl_state.is_god_acting();

    if (mutclass == MUTCLASS_INNATE)
        force_mutation = true;

    mutation_type mutat = which_mutation;

    if (!force_mutation)
    {
        // God gifts override all sources of mutation resistance other
        // than divine protection.
        if (!god_gift && _resist_mutation(mutclass, beneficial))
        {
            if (failMsg)
                mprf(MSGCH_MUTATION, "You feel odd for a moment.");
            return false;
        }

        // Zin's protection.
        if (have_passive(passive_t::resist_mutation)
            && x_chance_in_y(apply_pity(you.piety), piety_breakpoint(5)))
        {
            simple_god_message(" protects your body from mutation!");
            if ((mutclass == MUTCLASS_NORMAL) &&
                (!x_chance_in_y(you.gift_timeout,40))) {
                inc_gift_timeout(1);
            }
            return false;
        }
    }

    // Undead bodies don't mutate, they fall apart. -- bwr
    if (undead_mutation_rot(god_gift))
    {
        switch (mutclass)
        {
        case MUTCLASS_TEMPORARY:
            if (coinflip())
                return false;
            // fallthrough to normal mut
        case MUTCLASS_NORMAL:
            mprf(MSGCH_MUTATION, "Your body decomposes!");
            lose_stat(STAT_RANDOM, 1);
            return true;
        case MUTCLASS_INNATE:
            // You can't miss out on innate mutations just because you're
            // temporarily undead.
            break;
        default:
            die("bad fall through");
            return false;
        }
    }

    if (mutclass == MUTCLASS_NORMAL
        && (which_mutation == RANDOM_MUTATION
            || which_mutation == RANDOM_XOM_MUTATION)
        && x_chance_in_y(you.how_mutated(false, true), 15))
    {
        // God gifts override mutation loss due to being heavily
        // mutated.
        if (!one_chance_in(3) && !god_gift && !force_mutation)
            return false;
        else
            return delete_mutation(RANDOM_MUTATION, reason, failMsg,
                                   force_mutation, false);
    }

    switch (which_mutation)
    {
    case RANDOM_MUTATION:
    case RANDOM_GOOD_MUTATION:
    case RANDOM_BAD_MUTATION:
    case RANDOM_CORRUPT_MUTATION:
        mutat = _get_random_mutation(which_mutation, mutclass == MUTCLASS_TEMPORARY);
        break;
    case RANDOM_XOM_MUTATION:
        mutat = _get_random_xom_mutation(mutclass == MUTCLASS_TEMPORARY);
        break;
    case RANDOM_SLIME_MUTATION:
        mutat = _get_random_slime_mutation();
        break;
    case RANDOM_QAZLAL_MUTATION:
        mutat = _get_random_qazlal_mutation();
        break;
    default:
        break;
    }

    if (mutat == MUT_STRONG || mutat == MUT_CLEVER || mutat == MUT_AGILE)
    {
        _stat_potion(mutat);
        return true; // short-circuit due to not true mut.
    }

    if (!is_valid_mutation(mutat))
        return false;

    // [Cha] don't allow teleportitis in sprint
    if (mutat == MUT_TELEPORT && crawl_state.game_is_sprint())
        return false;

    if (physiology_mutation_conflict(mutat))
        return false;

    const mutation_def& mdef = _get_mutation_def(mutat);

    bool gain_msg = true;

    if (mutclass == MUTCLASS_INNATE)
    {
        // are there any non-innate instances to replace?  Prioritize temporary mutations over normal.
        // Temporarily decrement the mutation value so it can be silently regained in the while loop below.
        if (you.mutation[mutat] > you.innate_mutation[mutat])
        {
            if (you.temp_mutation[mutat] > 0)
            {
                you.temp_mutation[mutat]--;
                you.attribute[ATTR_TEMP_MUTATIONS]--;
                if (you.attribute[ATTR_TEMP_MUTATIONS] == 0)
                    you.attribute[ATTR_TEMP_MUT_XP] = 0;
            }
            you.mutation[mutat]--;
            mprf(MSGCH_MUTATION, "Your mutations feel more permanent.");
            take_note(Note(NOTE_PERM_MUTATION, mutat,
                    you.get_base_mutation_level(mutat), reason.c_str()));
            gain_msg = false;
        }
    }
    if (you.mutation[mutat] >= mdef.levels 
        || you.mutation[mutat] >= (you.innate_mutation[mutat] + 1)
            && !god_gift && !force_mutation && mutat != MUT_STATS
            && mutat != MUT_SUPPRESSION) // unlimited levels of stats and suppression
    {
        return false;
    }

    // God gifts and forced mutations clear away conflicting mutations.
    int rc = _handle_conflicting_mutations(mutat, god_gift || force_mutation,
                                           reason,
                                           mutclass == MUTCLASS_TEMPORARY);
    if (rc == 1)
        return true;
    if (rc == -1)
        return false;

    ASSERT(rc == 0);

    const unsigned int old_talents = your_talents(false).size();

    const int levels = (which_mutation == RANDOM_CORRUPT_MUTATION
                         || which_mutation == RANDOM_QAZLAL_MUTATION)
                       ? min(2, mdef.levels - you.get_base_mutation_level(mutat))
                       : 1;
    ASSERT(levels > 0); //TODO: is > too strong?

    int count = levels;

    while (count-- > 0)
    {
        // no fail condition past this point, so it is safe to do bookkeeping
        if (mutat != MUT_STATS)
            you.mutation[mutat]++;

        if (mutclass == MUTCLASS_TEMPORARY)
        {
            // do book-keeping for temporary mutations
            you.temp_mutation[mutat]++;
            you.attribute[ATTR_TEMP_MUTATIONS]++;
        }
        else if (mutclass == MUTCLASS_INNATE)
            you.innate_mutation[mutat]++;

        const int cur_base_level = you.get_base_mutation_level(mutat);
        gain_msg &= _pregain_effects(mutat, mutclass == MUTCLASS_TEMPORARY, which_mutation);

        // For all those scale mutations.
        you.redraw_armour_class = true;
        you.redraw_resists = true;

        notify_stat_change();

        if (gain_msg)
        {
            species_mutation_message msg = _spmut_msg(mutat);
            if (msg.mutation == MUT_NON_MUTATION)
                mprf(MSGCH_MUTATION, "%s", mdef.gain[cur_base_level - 1]);
            else
                mprf(MSGCH_MUTATION, "%s", msg.gain[cur_base_level - 1]);
        }

        _post_gain_effects(mutat);

        if (mutclass != MUTCLASS_TEMPORARY)
        {
            take_note(Note(NOTE_GET_MUTATION, mutat, cur_base_level,
                           reason.c_str()));
        }
        else
        {
            // only do this once regardless of how many levels got added
            you.attribute[ATTR_TEMP_MUT_XP] = temp_mutation_roll();
        }

        if (you.hp <= 0)
        {
            ouch(0, KILLED_BY_FRAILTY, MID_NOBODY,
                 make_stringf("gaining the %s mutation",
                              mutation_name(mutat)).c_str());
        }
    }

#ifdef USE_TILE_LOCAL
    if (your_talents(false).size() != old_talents)
    {
        tiles.layout_statcol();
        redraw_screen();
    }
#endif
    if (crawl_state.game_is_hints()
        && your_talents(false).size() > old_talents)
    {
        learned_something_new(HINT_NEW_ABILITY_MUT);
    }
#ifdef DEBUG
    if (mutclass != MUTCLASS_INNATE) // taken care of in perma_mutate. Skipping this here avoids validation issues in doing repairs.
        validate_mutations(false);
#endif
    return true;
}

static bool _post_loss_effects(mutation_type mutat, bool temp)
{
    bool lose_msg = true;

    switch (mutat)
    {
    case MUT_STATS:
        lose_msg = false;
        _unmutate_stats();
        break;

    case MUT_SUPPRESSION:
        lose_msg = false;
        _unsuppress_mutation(temp);
        break;

    case MUT_AMORPHOUS_BODY:
        _return_gear();
        break;

    case MUT_DRACONIAN_ENHANCER:
        lose_msg = false;
        mprf(MSGCH_MUTATION, "%s", _drac_enhancer_msg(-1).c_str());
        break;

    case MUT_HALF_DEATH:
        if (!you.mutation[mutat])
        {
            switch (you.undead_state(false))
            {
            default:
                mprf(MSGCH_MUTATION, "You return to your normal state of undeath.");
            case US_ALIVE:
                mprf(MSGCH_MUTATION, "You come fully back to life.");
                break;
            }
            lose_msg = false;
        }
        break;

    case MUT_SPIT_POISON:
        // Breathe poison replaces spit poison (so it takes the slot).
        if (you.mutation[mutat] < 2)
            abil_swap(ABIL_BREATHE_POISON, ABIL_SPIT_POISON);
        break;

    case MUT_DAYSTRIDER:
    case MUT_NIGHTSTALKER:
        update_vision_range();
        break;

    case MUT_TENGU_FLIGHT:
    case MUT_BIG_WINGS:
        lose_permafly_source();
        break;

    case MUT_ROOTS:
        if (you.attribute[ATTR_ROOTED])
            mprf("You are forceably ejected from the ground.");
        you.attribute[ATTR_ROOTED] = 0;
        break;

    case MUT_TENDRILS:
        remove_one_equip(EQ_RING_LEFT_TENDRIL, false, true);
        remove_one_equip(EQ_RING_RIGHT_TENDRIL, false, true);
        ash_check_bondage();
        break;

    case MUT_CYTOPLASMIC_SUSPENSION:
        remove_one_equip(EQ_CYTOPLASM, false, true);
        //fallthrough
    case MUT_HORNS:
    case MUT_ANTENNAE:
    case MUT_BEAK:
    case MUT_CLAWS:
    case MUT_HOOVES:
    case MUT_FROG_LEGS:
    case MUT_TALONS:
        // Recheck Ashenzari bondage in case our available slots changed.
        ash_check_bondage();
        break;

    case MUT_GELATINOUS_TAIL:
    {
        const mutation_def& mdef = _get_mutation_def(mutat);
        mprf(MSGCH_MUTATION, "%s%s", mdef.lose[0], you.has_tail(false) ? "back to its normal size." : "away completely.");
        remove_one_equip(EQ_BARDING, false, true);
        ash_check_bondage();
        lose_msg = false;
        break;
    }

    case MUT_UNSKILLED:
        _skill_rescale();
        break;

    default:
        break;
    }

    // For all those scale mutations.
    you.redraw_armour_class = true;
    you.redraw_resists = true;
    notify_stat_change();

    return lose_msg;
}

/*
 * Delete a single mutation level of fixed type `mutat`.
 * If `transient` is set, allow deleting temporary mutations, and prioritize them.
 * Note that if `transient` is true and there are no temporary mutations, this can delete non-temp mutations.
 * If `transient` is false, and there are only temp mutations, this will fail; otherwise it will delete a non-temp mutation.
 *
 * @mutat     the mutation to delete
 * @reason    why is it being deleted
 * @transient whether to allow (and prioritize) deletion of temporary mutations
 *
 * @return whether a mutation was deleted.
 */
static bool _delete_single_mutation_level(mutation_type mutat,
                                          const string &reason,
                                          bool transient,
                                          bool innate)
{
    bool was_transient = false;

    if (!innate)
    {
        // are there some non-innate mutations to delete?
        if (you.get_base_mutation_level(mutat, false, true, true) == 0)
            return false;

        if (you.has_temporary_mutation(mutat))
        {
            if (transient)
                was_transient = true;
            else if (you.get_base_mutation_level(mutat, false, false, true) == 0) // there are only temporary mutations to delete
                return false;

            // fall through: there is a non-temporary mutation level that can be deleted.
        }
    }

    if (mutat != MUT_STATS)
    {
        if (innate)
            you.innate_mutation[mutat]--;
        you.mutation[mutat]--;
    }

    const mutation_def& mdef = _get_mutation_def(mutat);
    const bool lose_msg = _post_loss_effects(mutat, was_transient);

    if (lose_msg)
    {
        species_mutation_message msg = _spmut_msg(mutat);
        if (msg.mutation == MUT_NON_MUTATION)
            mprf(MSGCH_MUTATION, "%s", mdef.lose[you.mutation[mutat]]);
        else
            mprf(MSGCH_MUTATION, "%s", msg.lose[you.mutation[mutat]]);
    }

    // Do post-mutation effects.
    if (mutat == MUT_FRAIL || mutat == MUT_ROBUST
        || mutat == MUT_RUGGED_BROWN_SCALES
        || mutat == MUT_ACID_WAVE)
    {
        calc_hp();
    }
    if (mutat == MUT_LOW_MAGIC || mutat == MUT_HIGH_MAGIC)
        calc_mp();

    if (was_transient)
    {
        --you.temp_mutation[mutat];
        --you.attribute[ATTR_TEMP_MUTATIONS];
    }
    else
        take_note(Note(NOTE_LOSE_MUTATION, mutat, you.mutation[mutat], reason));

    if (you.hp <= 0)
    {
        ouch(0, KILLED_BY_FRAILTY, MID_NOBODY,
             make_stringf("losing the %s mutation", mutation_name(mutat)).c_str());
    }

    return true;
}

/*
 * Delete a mutation level, accepting random mutation types and checking mutation resistance.
 * This will not delete temporary or innate mutations.
 *
 * @param which_mutation    a mutation, including random
 * @param reason            the reason for deletion
 * @param failMsg           whether to message the player on failure
 * @param force_mutation    whether to try to override certain cases where the mutation would otherwise fail
 * @param god_gift          is the mutation a god gift?  Will also override certain cases.
 * @param disallow_mismatch for random mutations, do we override good/bad designations in `which_mutation`? (??)
 *
 * @return true iff a mutation was applied.
 */
bool delete_mutation(mutation_type which_mutation, const string &reason,
                     bool failMsg,
                     bool force_mutation, bool god_gift,
                     bool disallow_mismatch)
{
    god_gift |= crawl_state.is_god_acting();

    mutation_type mutat = which_mutation;

    if (!force_mutation)
    {
        if (!god_gift)
        {
            if (you.get_mutation_level(MUT_MUTATION_RESISTANCE) > 1
                && (you.get_mutation_level(MUT_MUTATION_RESISTANCE) == 3
                    || coinflip()))
            {
                if (failMsg)
                    mprf(MSGCH_MUTATION, "You feel rather odd for a moment.");
                return false;
            }
        }

        if (undead_mutation_rot(god_gift))
            return false;
    }

    if (which_mutation == RANDOM_MUTATION
        || which_mutation == RANDOM_XOM_MUTATION
        || which_mutation == RANDOM_GOOD_MUTATION
        || which_mutation == RANDOM_BAD_MUTATION
        || which_mutation == RANDOM_NON_SLIME_MUTATION
        || which_mutation == RANDOM_CORRUPT_MUTATION
        || which_mutation == RANDOM_QAZLAL_MUTATION)
    {
        while (true)
        {
            if (one_chance_in(1000))
                return false;

            mutat = static_cast<mutation_type>(random2(NUM_MUTATIONS));

            if (which_mutation == RANDOM_NON_SLIME_MUTATION
                && is_slime_mutation(mutat))
            {
                continue;
            }

            // Check whether there is a non-innate level of `mutat` to delete
            if (you.get_base_mutation_level(mutat, false, true, true) == 0)
                continue;

            // MUT_ANTENNAE is 0, and you.attribute[] is initialized to 0.
            if (mutat && mutat == you.attribute[ATTR_APPENDAGE])
                continue;

            const mutation_def& mdef = _get_mutation_def(mutat);

            if (random2(10) >= mdef.weight && !is_slime_mutation(mutat))
                continue;

            const bool mismatch =
                (which_mutation == RANDOM_GOOD_MUTATION
                 && MUT_BAD(mdef))
                    || (which_mutation == RANDOM_BAD_MUTATION
                        && MUT_GOOD(mdef));

            if (mismatch && (disallow_mismatch || !one_chance_in(10)))
                continue;

            if (you.get_base_mutation_level(mutat, true, false, true) == 0)
                continue; // No non-transient mutations in this category to cure

            break;
        }
    }
    else if (which_mutation == RANDOM_SLIME_MUTATION)
    {
        mutat = _delete_random_slime_mutation();

        if (mutat == NUM_MUTATIONS)
            return false;
    }

    return _delete_single_mutation_level(mutat, reason, false); // won't delete temp mutations
}

/*
 * Delete all (non-innate) mutations.
 *
 * If you really need to delete innate mutations as well, have a look at `change_species_to` in species.cc.
 * Changing species to human, for example, is a safe way to clear innate mutations entirely. For a
 * demonspawn, you could also use wizmode code to set the level to 1.
 *
 * @return  Whether the function found mutations to delete.
 */
bool delete_all_mutations(const string &reason)
{
    for (int i = 0; i < NUM_MUTATIONS; ++i)
    {
        while (_delete_single_mutation_level(static_cast<mutation_type>(i), reason, true))
            ;
    }
    ASSERT(you.attribute[ATTR_TEMP_MUTATIONS] == 0);
    ASSERT(you.how_mutated(false, true, false) == 0);
    you.attribute[ATTR_TEMP_MUT_XP] = 0;

    return !you.how_mutated();
}

bool remove_slime_mutations()
{
    bool ret = false;
    for (int i = 0; i < NUM_MUTATIONS; ++i)
    {
        const mutation_type mut = static_cast<mutation_type>(i);
        if (is_slime_mutation(mut) && you.has_mutation(mut)
            && !(mut == MUT_CORE_MELDING && you.get_mutation_level(mut) > 1))
        {
            ret = true;
            you.innate_mutation[i] = 0;
            while (_delete_single_mutation_level(mut, "Jiyva's retribution", true))
                ; // for the messages
        }
    }
    return ret;
}

/*
 * Delete all temporary mutations.
 *
 * @return  Whether the function found mutations to delete.
 */
bool delete_all_temp_mutations(const string &reason)
{
    bool found = false;
    for (int i = 0; i < NUM_MUTATIONS; ++i)
    {
        while (you.has_temporary_mutation(static_cast<mutation_type>(i)))
            if (_delete_single_mutation_level(static_cast<mutation_type>(i), reason, true))
                found = true;
    }
    // the rest of the bookkeeping is handled in _delete_single_mutation_level
    you.attribute[ATTR_TEMP_MUT_XP] = 0;
    return found;
}

/*
 * Delete a single level of a random temporary mutation.
 * This function does not itself do XP-related bookkeeping; see `temp_mutation_wanes()`.
 *
 * @return          Whether the function found a mutation to delete.
 */
bool delete_temp_mutation()
{
    if (you.attribute[ATTR_TEMP_MUTATIONS] > 0)
    {
        mutation_type mutat = NUM_MUTATIONS;

        int count = 0;
        for (int i = 0; i < NUM_MUTATIONS; i++)
            if (you.has_temporary_mutation(static_cast<mutation_type>(i)) && one_chance_in(++count))
                mutat = static_cast<mutation_type>(i);

        if (_delete_single_mutation_level(mutat, "temp mutation expiry", true))
            return true;
    }

    return false;
}

void display_mutation_name(mutation_type mut, string &name)
{
    name = "";

    if (!is_valid_mutation(mut))
    {
        name = "invalid mutation";
        return;
    }

    if (mut == MUT_PSEUDOPODS)
    {
        switch (you.pseudopod_brand)
        {
        case SPWPN_MOLTEN:
            name = "scorching pseudopods";  break;
        case SPWPN_ACID:
            name = "corrosive pseudopods";  break;
        case SPWPN_ELECTROCUTION:
            name = "electric pseudopods";   break;
        case SPWPN_VENOM:
            name = "noxious pseudopods";    break;
        case SPWPN_VAMPIRISM:
            name = "vampiric pseudopods";   break;
        default:
            name = "buggy pseudopods";      break;
        }
    }

    else if (mut == MUT_STATS)
        _stat_mut_msg(name, false, you.mutated_stats[STAT_STR], you.mutated_stats[STAT_INT], you.mutated_stats[STAT_DEX], true);

    else if (mut == MUT_MINOR_MARTIAL_APT_BOOST || mut == MUT_MAJOR_MARTIAL_APT_BOOST || mut == MUT_DEFENSIVE_APT_BOOST)
    {
        // BCADNOTE: Brute force fix.
        skill_type skill = you.defence_skill;

        if (mut == MUT_MINOR_MARTIAL_APT_BOOST)
            skill = you.minor_skill;

        if (mut == MUT_MAJOR_MARTIAL_APT_BOOST)
            skill = you.major_skill;

        switch (skill)
        {
        case SK_ARMOUR:             name = "boosted armour aptitude";           break;
        case SK_AXES_HAMMERS:       name = "boosted axes & hammers aptitude";   break;
        case SK_BOWS:               name = "boosted bows aptitude";             break;
        case SK_CROSSBOWS:          name = "boosted crossbows aptitude";        break;
        case SK_DODGING:            name = "boosted dodging aptitude";          break;
        case SK_LONG_BLADES:        name = "boosted long blades aptitude";      break;
        case SK_MACES_STAVES:       name = "boosted maces & staves aptitude";   break;
        case SK_POLEARMS:           name = "boosted polearms aptitude";         break;
        case SK_SHIELDS:            name = "boosted shields aptitude";          break;
        case SK_SHORT_BLADES:       name = "boosted short blades aptitude";     break;
        case SK_SLINGS:             name = "boosted slings aptitude";           break;
        case SK_STEALTH:            name = "boosted stealth aptitude";          break;
        case SK_UNARMED_COMBAT:     name = "boosted unarmed combat aptitude";   break;
        case SK_WHIPS_FLAILS:       name = "boosted whips and flails aptitude"; break;
        default:                    name = "bad aptitude boost (bug)";          break;
        }
    }

    else if (mut == MUT_DRACONIAN_DEFENSE)
    {
        switch (you.drac_colour)
        {
        case DR_BLACK:              name = "Stlth+++";          break;
        case DR_BLOOD:              name = "rTorm, rHellfire";  break;
        case DR_BLUE:               name = "rElec";             break;
        case DR_BONE:               name = "AC+++, rShatter-";  break;
        default:
        case DR_BROWN:              name = "buggy mutation!";   break;
        case DR_CYAN:               name = "rCloud, rAir";      break;
        case DR_GOLDEN:             name = "rF+, rC+, rPois";   break;
        case DR_GREEN:              name = "rPois";             break;
        case DR_LIME:               name = "rCorr";             break;
        case DR_MAGENTA:            name = "rMsl";              break;
        case DR_OLIVE:              name = "rMut";              break;
        case DR_PEARL:            if (you.char_class == JOB_DEMONSPAWN)
                                        { name = "AC++, rHoly, rN+";  break; }
                                    name = "AC++, rN+++";       break;
        case DR_PINK:               name = "clarity";           break;
        case DR_PLATINUM:           name = "fast, rMut";        break;
        case DR_PURPLE:             name = "MR++";              break;
        case DR_RED:                name = "rF+";               break;
        case DR_SCINTILLATING:      name = "AC+, Chaos+";       break;
        case DR_SILVER:             name = "AC++, rMut";        break;
        case DR_TEAL:               name = "spectral";          break;
        case DR_WHITE:              name = "rC+";               break;
        }
    }

    else if (mut == MUT_DRACONIAN_ENHANCER)
    {
        switch (you.drac_colour)
        {
        case DR_BLACK:              name = "necromancy enhancer";                               break;
        case DR_BLOOD:              name = "necromancy, hexes and air enhancers";               break;
        case DR_BLUE:               name = "air enhancer";                                      break;
        case DR_BONE:               name = "charms and earth enhancers";                        break;
        case DR_CYAN:               name = "translocations enhancer";                           break;
        case DR_GOLDEN:             name = "fire, ice and poison enhancers";                    break;
        case DR_GREEN:              name = "poison enhancer";                                   break;
        case DR_LIME:               name = "transmutation enhancer";                            break;
        case DR_MAGENTA:            name = "charms enhancer";                                   break;
        case DR_OLIVE:              name = "poison and air enhancers";                          break;
        case DR_PEARL:              name = "charms, summoning and earth enhancers";             break;
        case DR_PINK:               name = "summoning enhancer";                                break;
        case DR_PLATINUM:           name = "translocation, transmutation and hexes enhancers";  break;
        case DR_PURPLE:             name = "hexes enhancer";                                    break;
        case DR_RED:                name = "fire enhancer";                                     break;
        case DR_SILVER:             name = "earth enhancer";                                    break;
        case DR_TEAL:               name = "translocation and transmutation enhancers";         break;
        case DR_WHITE:              name = "ice enhancer";                                      break;
        case DR_SCINTILLATING:      name = "chaos magic, random archmage";                      break;
        default:    // Shouldn't display ever; hopefully.
        case DR_BROWN:              name = "bugginess.";                                        break;
        }
    }

    else if (mut == MUT_HORNS && you.get_mutation_level(MUT_HORNS) > 1)
        name = "horns, retaliatory headbutt";

    else
    {
        species_mutation_message msg = _spmut_msg(mut);
        if (msg.mutation == MUT_NON_MUTATION)
            name = _get_mutation_def(mut).short_desc;
        else
            name = msg.short_desc;
    }
}

// If for_display is false ignores species-specific messaging to give a neat version for use in wizmode etc.
const char* mutation_name(mutation_type mut, bool allow_category)
{
    if (allow_category && mut >= CATEGORY_MUTATIONS && mut < MUT_NON_MUTATION)
        return _get_category_mutation_def(mut).short_desc;

    // note -- this can produce crashes if fed invalid mutations, e.g. if allow_category is false and mut is a category mutation
    if (!is_valid_mutation(mut))
        return nullptr;

    return _get_mutation_def(mut).short_desc;
}

const char* category_mutation_name(mutation_type mut)
{
    if (mut < CATEGORY_MUTATIONS || mut >= MUT_NON_MUTATION)
        return nullptr;
    return _get_category_mutation_def(mut).short_desc;
}

/*
 * Given some name, return a mutation type. Tries to match the short description as found in `mutation-data.h`.
 * If `partial_matches` is set, it will fill the vector with any partial matches it finds. If there is exactly one,
 * will return this mutation, otherwise, will fail.
 *
 * @param allow_category    whether to include category mutation types (e.g. RANDOM_GOOD)
 * @param partial_matches   an optional pointer to a vector, in case the consumer wants to do something
 *                          with the partial match results (e.g. show them to the user). If this is `nullptr`,
 *                          will accept only exact matches.
 *
 * @return the mutation type if succesful, otherwise NUM_MUTATIONS if it can't find a single match.
 */
mutation_type mutation_from_name(string name, bool allow_category, vector<mutation_type> *partial_matches)
{
    mutation_type mutat = NUM_MUTATIONS;

    string spec = lowercase_string(name);

    if (allow_category)
    {
        for (int i = CATEGORY_MUTATIONS; i < MUT_NON_MUTATION; ++i)
        {
            mutation_type mut = static_cast<mutation_type>(i);
            const char* mut_name_c = category_mutation_name(mut);
            if (!mut_name_c)
                continue;
            const string mut_name = lowercase_string(mut_name_c);

            if (spec == mut_name)
                return mut; // note, won't fully populate partial_matches

            if (partial_matches && strstr(mut_name.c_str(), spec.c_str()))
                partial_matches->push_back(mut);
        }
    }

    for (int i = 0; i < NUM_MUTATIONS; ++i)
    {
        mutation_type mut = static_cast<mutation_type>(i);
        const char *mut_name_c = mutation_name(mut);
        if (!mut_name_c)
            continue;
        const string mut_name = lowercase_string(mut_name_c);

        if (spec == mut_name)
        {
            mutat = mut;
            break;
        }

        if (partial_matches && strstr(mut_name.c_str(), spec.c_str()))
            partial_matches->push_back(mut);
    }

    // If only one matching mutation, use that.
    if (partial_matches && mutat == NUM_MUTATIONS && partial_matches->size() == 1)
        mutat = (*partial_matches)[0];
    return mutat;
}

/**
 * A summary of what the next level of a mutation does.
 *
 * @param mut   The mutation_type in question; e.g. MUT_FRAIL.
 * @return      The mutation's description, helpfully trimmed.
 *              e.g. "you are frail (-10% HP)".
 */
string mut_upgrade_summary(mutation_type mut)
{
    if (!is_valid_mutation(mut))
        return nullptr;

    string mut_desc =
        lowercase_first(mutation_desc(mut, you.mutation[mut] + 1));
    strip_suffix(mut_desc, ".");
    return mut_desc;
}

int mutation_max_levels(mutation_type mut)
{
    if (!is_valid_mutation(mut))
        return 0;

    return _get_mutation_def(mut).levels;
}

static string _suppress_msg(bool colour)
{
    vector<string> mut_names;
    vector<string> temp_mut_names;
    string retval = "";
    for (int i = 0; i < NUM_MUTATIONS; i++)
    {
        if (you.suppressed_mutation[i])
        {
            const mutation_type mut = static_cast<mutation_type>(i);
            string name = "";
            display_mutation_name(mut, name);

            if (you.suppressed_mutation[i] > 1)
                temp_mut_names.emplace_back(name);
            else
                mut_names.emplace_back(name);
        }
    }
    if (mut_names.size())
    {
        string suppressed = mut_names.size() > 1 ? comma_separated_line(mut_names.begin(), mut_names.end()) : mut_names[0];
        retval = make_stringf("%sYour %s mutation%s suppressed.%s%s", colour ? "<lightgrey>" : "", 
            suppressed.c_str(), mut_names.size() > 1 ? "s are" : " is", colour ? "</lightgrey>" : "", temp_mut_names.size() ? "\n" : "");
    }
    if (temp_mut_names.size())
    {
        string suppressed = temp_mut_names.size() > 1 ? comma_separated_line(temp_mut_names.begin(), temp_mut_names.end()) : temp_mut_names[0];
        retval = make_stringf("%s[Your %s mutation%s transiently suppressed.]%s", colour ? "<magenta>" : "",
            suppressed.c_str(), temp_mut_names.size() > 1 ? "s are" : " is", colour ? "</magenta>" : "");
    }
    return retval;
}

// Return a string describing the mutation.
// If colour is true, also add the colour annotation.
string mutation_desc(mutation_type mut, int level, bool colour,
        bool is_sacrifice)
{
    // Ignore the player's forms, etc.
    const bool ignore_player = (level != -1);

    const int curlvl = you.get_base_mutation_level(mut);

    const mutation_activity_type active = mutation_activity_level(mut);
    const bool partially_active = (active == mutation_activity_type::PARTIAL)
        || curlvl != 0 && curlvl < you.get_base_mutation_level(mut, true, true, true, false);
    const bool fully_inactive = (active == mutation_activity_type::INACTIVE)
        || curlvl == 0;

    const bool temporary = you.has_temporary_mutation(mut);

    // level == -1 means default action of current level
    if (level == -1)
    {
        if (!fully_inactive)
            level = you.get_mutation_level(mut);
        else // give description of fully active mutation
            level = you.get_base_mutation_level(mut, true, true, true, false);
    }

    string result = "";

    const mutation_def& mdef = _get_mutation_def(mut);
    const species_mutation_message msg = _spmut_msg(mut);

    if (mut == MUT_STATS)
        _stat_mut_msg(result, false, you.mutated_stats[STAT_STR], you.mutated_stats[STAT_INT], you.mutated_stats[STAT_DEX], false);
    else if (mut == MUT_HALF_DEATH)
    {
        string base_msg;
        switch (you.undead_state(false))
        {
        default:
        case US_SEMI_ALIVE:
            base_msg = "You require no food and take half damage from torment. You may still use Necromantic secrets and transmutations forms.";
            break;
        case US_SEMI_UNDEAD:
            base_msg = "You are able to benefit from potions like a living creature and may once more use transmutations magic. "
                "You still require no food and cannot be raised to a blood rage.";
            break;
        }
        result = make_stringf("You are in an odd twilight state between life and undeath. %s%s",
            base_msg.c_str(), you.mutation[mut] > 1 ? "\nYou exude an unnatural aura that makes creatures very uncomfortable and unable to regenerate." : "");
    }
    else if (mut == MUT_PSEUDOPODS)
        result = _pseudopod_message(false);
    else if (mut == MUT_ICEMAIL)
    {
        ostringstream ostr;
        ostr << mdef.have[0] << player_icemail_armour_class() << ")";
        result = ostr.str();
    }
    else if (mut == MUT_DISTORTION_FIELD || mut == MUT_ICY_BLUE_SCALES || mut == MUT_IRIDESCENT_SCALES
        || mut == MUT_RUGGED_BROWN_SCALES || mut == MUT_MOLTEN_SCALES || mut == MUT_SLIMY_GREEN_SCALES
        || mut == MUT_THIN_METALLIC_SCALES || mut == MUT_YELLOW_SCALES || mut == MUT_ROUGH_BLACK_SCALES
        || mut == MUT_THIN_SKELETAL_STRUCTURE || mut == MUT_STURDY_FRAME)
    {
        ostringstream ostr;

        int bonus = you.ac_change_from_mutation(mut) / 100;

        if (!bonus)
            bonus = you.char_class == JOB_DEMONSPAWN ? you.get_experience_level() / 3 : 3;

        if (mut == MUT_THIN_SKELETAL_STRUCTURE)
            bonus += 3;

        if (msg.mutation == MUT_NON_MUTATION)
            ostr << mdef.have[level - 1] << bonus << ")";
        else
            ostr << msg.have[level - 1] << bonus << ")";
        result = ostr.str();
    }
    else if (mut == MUT_LARGE_BONE_PLATES)
    {
        ostringstream ostr;

        int bonus = (you.char_class == JOB_DEMONSPAWN) ? (you.get_experience_level() / 3 * 2) : 6;

        if (msg.mutation == MUT_NON_MUTATION)
        {
            const char *arms;
            if (you.species == SP_FELID)
                arms = "legs";
            else if (you.species == SP_OCTOPODE)
                arms = "tentacles";
            else
                arms = "arms";

            ostr << replace_all(mdef.have[level - 1], "arms", arms).c_str() << bonus << ")";
        }
        else
            ostr << msg.have[level - 1] << bonus << ")";

        result = ostr.str();
    }
    else if (mut == MUT_BRANCHES)
    {
        ostringstream ostr;

        const int bonus = you.branch_SH(true);

        if (msg.mutation == MUT_NON_MUTATION)
            ostr << mdef.have[level - 1] << bonus << ")";
        else
            ostr << msg.have[level - 1] << bonus << ")";

        result = ostr.str();
    }
    else if (mut == MUT_SANGUINE_ARMOUR)
    {
        ostringstream ostr;
        ostr << mdef.have[level - 1] << sanguine_armour_bonus() / 100 << ")";
        result = ostr.str();
    }
    else if (mut == MUT_DRACONIAN_DEFENSE)
        result = _drac_def_msg();
    else if (mut == MUT_DRACONIAN_ENHANCER)
        result = _drac_enhancer_msg(0);
    else if (mut == MUT_SUPPRESSION)
        return _suppress_msg(colour && !ignore_player);
    else if (mut == MUT_MINOR_MARTIAL_APT_BOOST)
    {
        ostringstream ostr;
        ostr << mdef.have[0] << skill_name(you.minor_skill) << " (Aptitude +3).";
        result = ostr.str();
    }
    else if (mut == MUT_DEFENSIVE_APT_BOOST)
    {
        ostringstream ostr;
        ostr << mdef.have[0] << skill_name(you.defence_skill) << " (Aptitude +3).";
        result = ostr.str();
    }
    else if (mut == MUT_MAJOR_MARTIAL_APT_BOOST)
    {
        ostringstream ostr;
        string x = level > 1 ? " (Aptitude +5)." : " (Aptitude +3).";
        ostr << mdef.have[level - 1] << skill_name(you.major_skill) << x;
        result = ostr.str();
    }
    else if (!ignore_player && you.species == SP_FELID && mut == MUT_CLAWS)
        result = "You have sharp claws.";
    else if (have_passive(passive_t::no_mp_regen) && mut == MUT_ANTIMAGIC_BITE)
        result = "Your bite disrupts the magic of your enemies.";
    else if (result.empty() && level > 0)
    {
        if (msg.mutation == MUT_NON_MUTATION)
            result = mdef.have[level - 1];
        else
            result = msg.have[level - 1];
    }

    if (!ignore_player)
    {
        if (fully_inactive)
        {
            result = "((" + result + "))";
            ++_num_full_suppressed;
        }
        else if (partially_active)
        {
            result = "(" + result + ")";
            ++_num_part_suppressed;
        }
    }

    if (temporary)
    {
        result = "[" + result + "]";
        ++_num_transient;
    }

    if (colour)
    {
        const char* colourname = (MUT_BAD(mdef) ? "red" : "lightgrey");
        const bool permanent   = you.has_innate_mutation(mut);

        if (mut == MUT_STATS)
        {
            if (you.mutated_stats[STAT_STR] + you.mutated_stats[STAT_INT] + you.mutated_stats[STAT_DEX] >= 0)
                colourname = "lightgrey"; // Otherwise it was already set to red by the default.
        }

        if (permanent)
        {
            const bool ds = (you.species == SP_DEMONSPAWN || you.char_class == JOB_DEMONSPAWN);
            bool demonspawn = false;

            if (ds)
            {
                for (unsigned int i = 0; i < you.demonic_traits.size(); i++)
                {
                    if (you.demonic_traits[i].mutation == mut &&
                        you.demonic_traits[i].level_gained <= you.experience_level)
                    {
                        demonspawn = true;
                        i += 1000;
                    }
                }
            }

            const bool extra = you.get_base_mutation_level(mut, false, true, true, false) > 0;

            if (fully_inactive || (mut == MUT_COLD_BLOODED && player_res_cold(false) > 0)
                || (mut == MUT_DEFORMED && (you.get_mutation_level(MUT_CORE_MELDING) || you.get_mutation_level(MUT_AMORPHOUS_BODY))))
            {
                colourname = "darkgrey";
            }
            else if (is_sacrifice)
                colourname = "lightred";
            else if (is_slime_mutation(mut))
                colourname = partially_active ? "lightgreen" : "green";
            else if (partially_active)
                colourname = demonspawn ? "yellow"    : "blue";
            else if (extra)
                colourname = demonspawn ? "lightcyan" : "cyan";
            else
                colourname = demonspawn ? "cyan"      : "lightblue";
        }
        else if (fully_inactive)
            colourname = "darkgrey";
        else if (partially_active)
            colourname = "brown";
        else if (you.form == transformation::appendage && you.attribute[ATTR_APPENDAGE] == mut)
            colourname = "lightgreen";
        else if (temporary)
            colourname = (you.get_base_mutation_level(mut, true, false, true, false) > 0) ?
                         "lightmagenta" : "magenta";

        // Build the result
        ostringstream ostr;
        ostr << '<' << colourname << '>' << result
             << "</" << colourname << '>';
        result = ostr.str();
    }

    return result;
}

// The "when" numbers indicate the range of times in which the mutation tries
// to place itself; it will be approximately placed between when% and
// (when + 100)% of the way through the mutations. For example, you should
// usually get all your body slot mutations in the first 2/3 of your
// mutations and you should usually only start your tier 3 facet in the second
// half of your mutations. See _order_ds_mutations() for details.
static const facet_def _demon_facets[] =
{
    // Body Slot facets
    { 0, { MUT_NON_MUTATION, MUT_CLAWS, MUT_CLAWS },
      { -33, -33, -33 } },
    { 0, { MUT_NON_MUTATION, MUT_NON_MUTATION, MUT_HORNS },
      { -33, -33, -33 } },
    { 0, { MUT_NON_MUTATION, MUT_NON_MUTATION, MUT_ANTENNAE },
      { -33, -33, -33 } },
    { 0, { MUT_NON_MUTATION, MUT_NON_MUTATION, MUT_HOOVES },
      { -33, -33, -33 } },
    { 0, { MUT_NON_MUTATION, MUT_FROG_LEGS, MUT_FROG_LEGS },
      { -33, -33, -33 } },
    { 0, { MUT_NON_MUTATION, MUT_NON_MUTATION, MUT_TALONS },
      { -33, -33, -33 } },
    // Scale mutations
    { 1, { MUT_NON_MUTATION, MUT_NON_MUTATION, MUT_DISTORTION_FIELD },
      { -33, -33, 0 } },
    { 1, { MUT_NON_MUTATION, MUT_NON_MUTATION, MUT_ICY_BLUE_SCALES },
      { -33, -33, 0 } },
    { 1, { MUT_NON_MUTATION, MUT_NON_MUTATION, MUT_IRIDESCENT_SCALES },
      { -33, -33, 0 } },
    { 1, { MUT_NON_MUTATION, MUT_NON_MUTATION, MUT_LARGE_BONE_PLATES },
      { -33, -33, 0 } },
    { 1, { MUT_NON_MUTATION, MUT_NON_MUTATION, MUT_MOLTEN_SCALES },
      { -33, -33, 0 } },
    { 1, { MUT_NON_MUTATION, MUT_NON_MUTATION, MUT_RUGGED_BROWN_SCALES },
      { -33, -33, 0 } },
    { 1, { MUT_NON_MUTATION, MUT_NON_MUTATION, MUT_SLIMY_GREEN_SCALES },
      { -33, -33, 0 } },
    { 1, { MUT_NON_MUTATION, MUT_NON_MUTATION, MUT_THIN_METALLIC_SCALES },
      { -33, -33, 0 } },
    { 1, { MUT_NON_MUTATION, MUT_NON_MUTATION, MUT_THIN_SKELETAL_STRUCTURE },
      { -33, -33, 0 } },
    { 1, { MUT_NON_MUTATION, MUT_NON_MUTATION, MUT_YELLOW_SCALES },
      { -33, -33, 0 } },
    { 1, { MUT_NON_MUTATION, MUT_NON_MUTATION, MUT_STURDY_FRAME },
      { -33, -33, 0 } },
    { 1, { MUT_NON_MUTATION, MUT_NON_MUTATION, MUT_SANGUINE_ARMOUR },
      { -33, -33, 0 } },
    // Tier 2 facets
    { 2, { MUT_HEAT_RESISTANCE, MUT_FLAME_CLOUD_IMMUNITY, MUT_IGNITE_BLOOD },
      { -33, 0, 0 } },
    { 2, { MUT_COLD_RESISTANCE, MUT_FREEZING_CLOUD_IMMUNITY, MUT_ICEMAIL },
      { -33, 0, 0 } },
    { 2, { MUT_POWERED_BY_DEATH, MUT_POWERED_BY_DEATH, MUT_POWERED_BY_DEATH },
      { -33, 0, 0 } },
    { 2, { MUT_DEMONIC_GUARDIAN, MUT_DEMONIC_GUARDIAN, MUT_DEMONIC_GUARDIAN },
      { -66, 17, 50 } },
    { 2, { MUT_NIGHTSTALKER, MUT_NIGHTSTALKER, MUT_NIGHTSTALKER },
      { -33, 0, 0 } },
    { 2, { MUT_SPINY, MUT_SPINY, MUT_SPINY },
      { -33, 0, 0 } },
    { 2, { MUT_POWERED_BY_PAIN, MUT_POWERED_BY_PAIN, MUT_POWERED_BY_PAIN },
      { -33, 0, 0 } },
    { 2, { MUT_ROT_IMMUNITY, MUT_FOUL_STENCH, MUT_FOUL_STENCH },
      { -33, 0, 0 } },
    { 2, { MUT_MANA_SHIELD, MUT_MANA_REGENERATION, MUT_MANA_LINK },
      { -33, 0, 0 } },
    // Tier 3 facets
    { 3, { MUT_HEAT_RESISTANCE, MUT_FLAME_CLOUD_IMMUNITY, MUT_HURL_HELLFIRE },
      { 50, 50, 50 } },
    { 3, { MUT_COLD_RESISTANCE, MUT_FREEZING_CLOUD_IMMUNITY, MUT_PASSIVE_FREEZE },
      { 50, 50, 50 } },
    { 3, { MUT_ROBUST, MUT_ROBUST, MUT_ROBUST },
      { 50, 50, 50 } },
    { 3, { MUT_NEGATIVE_ENERGY_RESISTANCE, MUT_STOCHASTIC_TORMENT_RESISTANCE,
           MUT_BLACK_MARK },
      { 50, 50, 50 } },
    { 3, { MUT_AUGMENTATION, MUT_AUGMENTATION, MUT_AUGMENTATION },
      { 50, 50, 50 } },
};

static bool _works_at_tier(const facet_def& facet, int tier)
{
    return facet.tier == tier;
}

typedef decltype(facet_def().muts) mut_array_t;
static bool _slot_is_unique(const mut_array_t &mut,
                            set<const facet_def *> facets_used)
{
    set<equipment_type> eq;

    // find the equipment slot(s) used by mut
    for (const body_facet_def &facet : _body_facets)
    {
        for (mutation_type slotmut : mut)
            if (facet.mut == slotmut)
                eq.insert(facet.eq);
    }

    if (eq.empty())
        return true;

    for (const facet_def *used : facets_used)
    {
        for (const body_facet_def &facet : _body_facets)
            if (facet.mut == used->muts[0] && eq.count(facet.eq))
                return false;
    }

    return true;
}

static vector<demon_mutation_info> _select_ds_mutations()
{
    int ct_of_tier[] = { 1, 1, 2, 1 };

try_again:
    vector<demon_mutation_info> ret;

    ret.clear();
    int absfacet = 0;
    int ice_elemental = 0;
    int fire_elemental = 0;
    int cloud_producing = 0;

    set<const facet_def *> facets_used;

    for (int tier = ARRAYSZ(ct_of_tier) - 1; tier >= 0; --tier)
    {
        for (int nfacet = 0; nfacet < ct_of_tier[tier]; ++nfacet)
        {
            const facet_def* next_facet;

            do
            {
                next_facet = &RANDOM_ELEMENT(_demon_facets);
            }
            while (!_works_at_tier(*next_facet, tier)
                   || facets_used.count(next_facet)
                   || !_slot_is_unique(next_facet->muts, facets_used)
                   || physiology_mutation_conflict(next_facet->muts[2], true));

            facets_used.insert(next_facet);

            for (int i = 0; i < 3; ++i)
            {
                mutation_type m = next_facet->muts[i];

                if (m != MUT_NON_MUTATION)
                    ret.emplace_back(m, next_facet->when[i], absfacet);

                if (m == MUT_COLD_RESISTANCE)
                    ice_elemental++;

                if (m == MUT_HEAT_RESISTANCE)
                    fire_elemental++;

                if (m == MUT_ROT_IMMUNITY || m == MUT_IGNITE_BLOOD)
                    cloud_producing++;
            }

            ++absfacet;
        }
    }

    if (ice_elemental + fire_elemental > 1)
        goto try_again;

    if (cloud_producing > 1)
        goto try_again;

    return ret;
}

static vector<mutation_type>
_order_ds_mutations(vector<demon_mutation_info> muts)
{
    vector<mutation_type> out;
    vector<int> times;
    FixedVector<int, 1000> time_slots;
    time_slots.init(-1);
    for (unsigned int i = 0; i < muts.size(); i++)
    {
        int first = max(0, muts[i].when);
        int last = min(100, muts[i].when + 100);
        int k;
        do
        {
            k = 10 * first + random2(10 * (last - first));
        }
        while (time_slots[k] >= 0);
        time_slots[k] = i;
        times.push_back(k);

        // Don't reorder mutations within a facet.
        for (unsigned int j = i; j > 0; j--)
        {
            if (muts[j].facet == muts[j-1].facet && times[j] < times[j-1])
            {
                int earlier = times[j];
                int later = times[j-1];
                time_slots[earlier] = j-1;
                time_slots[later] = j;
                times[j-1] = earlier;
                times[j] = later;
            }
            else
                break;
        }
    }

    for (int time = 0; time < 1000; time++)
        if (time_slots[time] >= 0)
            out.push_back(muts[time_slots[time]].mut);

    return out;
}

static vector<player::demon_trait>
_schedule_ds_mutations(vector<mutation_type> muts)
{
    list<mutation_type> muts_left(muts.begin(), muts.end());

    list<int> slots_left;

    vector<player::demon_trait> out;

    for (int level = 2; level <= 27; ++level)
        slots_left.push_back(level);

    while (!muts_left.empty())
    {
        if (out.empty() // always give a mutation at XL 2
            || x_chance_in_y(muts_left.size(), slots_left.size()))
        {
            player::demon_trait dt;

            dt.level_gained = slots_left.front();
            dt.mutation = muts_left.front();

            dprf("Demonspawn will gain %s at level %d",
                    _get_mutation_def(dt.mutation).short_desc, dt.level_gained);

            out.push_back(dt);

            muts_left.pop_front();
        }
        slots_left.pop_front();
    }

    return out;
}

void roll_demonspawn_mutations()
{
    // intentionally create the subgenerator either way, so that this has the
    // same impact on the current main rng for all chars.
    rng::subgenerator ds_rng;

    if (you.char_class != JOB_DEMONSPAWN)
        return;
    you.demonic_traits = _schedule_ds_mutations(
                         _order_ds_mutations(
                         _select_ds_mutations()));
}

static bool _is_ranged_skill(skill_type skill)
{
    if (skill == SK_BOWS || skill == SK_CROSSBOWS || skill == SK_SLINGS)
        return true;
    return false;
}

// Sets up variables only used by the Draconian pseudomutations.
void draconian_setup()
{
    if (!species_is_draconian(you.species))
        return;

    // Only case where you'd get here at higher than XL 7 is importing an old save (rare) but still.
    if (you.experience_level < 7)
        you.drac_colour = DR_BROWN;

    you.major_first = coinflip();

    you.major_skill = random_choose(SK_LONG_BLADES, SK_AXES_HAMMERS, SK_MACES_STAVES, SK_WHIPS_FLAILS, SK_SLINGS,
        SK_BOWS, SK_CROSSBOWS);

    you.minor_skill = you.major_skill;

    while (you.major_skill == you.minor_skill)
    {
        you.minor_skill = random_choose(SK_SHORT_BLADES, SK_LONG_BLADES, SK_AXES_HAMMERS, SK_MACES_STAVES, SK_WHIPS_FLAILS,
            SK_SLINGS, SK_BOWS, SK_CROSSBOWS, SK_UNARMED_COMBAT);
    }

    you.defence_skill = random_choose(SK_DODGING, SK_STEALTH, SK_SHIELDS);

    if (_is_ranged_skill(you.major_skill) && _is_ranged_skill(you.minor_skill))
    {
        if (coinflip())
        {
            you.minor_skill = random_choose(SK_SHORT_BLADES, SK_LONG_BLADES, SK_AXES_HAMMERS, SK_MACES_STAVES, SK_WHIPS_FLAILS,
                SK_UNARMED_COMBAT);
        }
        else
            you.major_skill = random_choose(SK_LONG_BLADES, SK_AXES_HAMMERS, SK_MACES_STAVES, SK_WHIPS_FLAILS);
    }
}

bool perma_mutate(mutation_type which_mut, int how_much, const string &reason)
{
    ASSERT(is_valid_mutation(which_mut));

    int cap = get_mutation_cap(which_mut);
    how_much = min(how_much, cap);

    int rc = 1;
    // clear out conflicting mutations
    int count = 0;
    while (rc == 1 && ++count < 100)
        rc = _handle_conflicting_mutations(which_mut, true, reason, false, true);
    if (rc != 0)
        return true;

    int levels = 0;
    while (how_much-- > 0)
    {
        dprf("Perma Mutate %s: cap %d, total %d, innate %d", mutation_name(which_mut), cap,
            you.get_base_mutation_level(which_mut), you.get_innate_mutation_level(which_mut));
        if (you.get_base_mutation_level(which_mut, true, false, false) < cap
            && !mutate(which_mut, reason, false, true, false, false, MUTCLASS_INNATE))
        {
            dprf("Innate mutation failed.");
            break;
        }
        levels++;
    }

#ifdef DEBUG
    // don't validate permamutate directly on level regain; this is so that wizmode level change
    // functions can work correctly.
    if (you.experience_level >= you.max_level)
        validate_mutations(false);
#endif
    return levels > 0;
}

bool temp_mutate(mutation_type which_mut, const string &reason)
{
    return mutate(which_mut, reason, false, false, false, false, MUTCLASS_TEMPORARY);
}

int temp_mutation_roll()
{
    return min(you.experience_level, 17) * (500 + roll_dice(5, 500)) / 17;
}

bool temp_mutation_wanes()
{
    const int starting_tmuts = you.attribute[ATTR_TEMP_MUTATIONS];
    if (starting_tmuts == 0)
        return false;

    int num_remove = min(starting_tmuts,
        max(starting_tmuts * 5 / 12 - random2(3),
        1 + random2(3)));

    mprf(MSGCH_DURATION, "You feel the corruption within you wane %s.",
        (num_remove >= starting_tmuts ? "completely" : "somewhat"));

    for (int i = 0; i < num_remove; ++i)
        delete_temp_mutation(); // chooses randomly

    if (you.attribute[ATTR_TEMP_MUTATIONS] > 0)
        you.attribute[ATTR_TEMP_MUT_XP] += temp_mutation_roll();
    else
        you.attribute[ATTR_TEMP_MUT_XP] = 0;
    ASSERT(you.attribute[ATTR_TEMP_MUTATIONS] < starting_tmuts);
    return true;
}

/**
 * How mutated is the player?
 *
 * @param innate Whether to count innate mutations (default false).
 * @param levels Whether to add up mutation levels, as opposed to just counting number of mutations (default false).
 * @param temp   Whether to count temporary mutations (default true).
 * @param ds     Whether to count innate demonspawn mutations, ignored if innate is true (default false).
 * @param normal Whether to count normal non-innate mutations. (default true).
 * @return Either the number of matching mutations, or the sum of their
 *         levels, depending on \c levels
 */
int player::how_mutated(bool innate, bool levels, bool temp, bool ds, bool normal) const
{
    int result = 0;

    for (int i = 0; i < NUM_MUTATIONS; ++i)
    {
        if (you.mutation[i])
        {
            if (ds && !innate)
            {
                const int mut_level = get_base_mutation_level(static_cast<mutation_type>(i), true, false, false);

                if (mut_level)
                {
                    for (unsigned int j = 0; j < you.demonic_traits.size(); j++)
                    {
                        if (you.demonic_traits[j].mutation == i &&
                            you.demonic_traits[j].level_gained <= you.experience_level)
                        {
                            if (levels)
                                result += mut_level;
                            else 
                                result++;
                            j += 1000;
                        }
                    }
                }
            }

            const int mut_level = get_base_mutation_level(static_cast<mutation_type>(i), innate, temp, normal);

            if (levels)
                result += mut_level;
            else if (mut_level > 0)
                result++;
        }
        if (innate && you.props.exists("num_sacrifice_muts"))
            result -= you.props["num_sacrifice_muts"].get_int();
    }

    return result;
}

// Return whether current tension is balanced
static bool _balance_demonic_guardian()
{
    // if tension is unfavorably high, perhaps another guardian should spawn
    const int mutlevel = you.get_mutation_level(MUT_DEMONIC_GUARDIAN);
    const int tension = get_tension(GOD_NO_GOD);
    return tension*3/4 <= mutlevel*6 + random2(mutlevel*mutlevel*2);
}

// Primary function to handle and balance demonic guardians, if the tension
// is unfavorably high and a guardian was not recently spawned, a new guardian
// will be made, if tension is below a threshold (determined by the mutations
// level and a bit of randomness), guardians may be dismissed in
// _balance_demonic_guardian()
void check_demonic_guardian()
{
    // Players hated by all monsters don't get guardians, so that they aren't
    // swarmed by hostile executioners whenever things get rough.
    if (you.get_mutation_level(MUT_NO_LOVE))
        return;

    const int mutlevel = you.get_mutation_level(MUT_DEMONIC_GUARDIAN);

    if (!_balance_demonic_guardian() &&
        you.duration[DUR_DEMONIC_GUARDIAN] == 0)
    {
        monster_type mt;

        switch (mutlevel)
        {
        case 1:
            mt = random_choose(MONS_QUASIT, MONS_WHITE_IMP, MONS_UFETUBUS,
                               MONS_IRON_IMP, MONS_SHADOW_IMP);
            break;
        case 2:
            mt = random_choose(MONS_ORANGE_DEMON, MONS_ICE_DEVIL,
                               MONS_SOUL_EATER, MONS_SMOKE_DEMON,
                               MONS_SIXFIRHY);
            break;
        case 3:
            mt = random_choose(MONS_EXECUTIONER, MONS_BALRUG, MONS_REAPER,
                               MONS_CACODEMON, MONS_LOROCYPROCA);
            break;
        default:
            die("Invalid demonic guardian level: %d", mutlevel);
        }

        monster *guardian = create_monster(
            mgen_data(mt, BEH_FRIENDLY, you.pos(), MHITYOU,
                      MG_FORCE_BEH | MG_AUTOFOE).set_summoned(&you, 2, 0));

        if (!guardian)
            return;

        guardian->flags |= MF_NO_REWARD;
        guardian->flags |= MF_DEMONIC_GUARDIAN;

        guardian->add_ench(ENCH_LIFE_TIMER);

        // no more guardians for mutlevel+1 to mutlevel+20 turns
        you.duration[DUR_DEMONIC_GUARDIAN] = 10*(mutlevel + random2(20));
    }
}

/**
 * Update the map knowledge based on any monster detection sources the player
 * has.
 */
void check_monster_detect()
{
    int radius = player_monster_detect_radius();
    if (radius <= 0)
        return;

    for (radius_iterator ri(you.pos(), radius, C_SQUARE); ri; ++ri)
    {
        monster* mon = monster_at(*ri);
        map_cell& cell = env.map_knowledge(*ri);
        if (!mon)
        {
            if (cell.detected_monster())
                cell.clear_monster();
            continue;
        }
        if (mons_is_firewood(*mon))
            continue;

        // [ds] If the PC remembers the correct monster at this
        // square, don't trample it with MONS_SENSED. Forgetting
        // legitimate monster memory affects travel, which can
        // path around mimics correctly only if it can actually
        // *see* them in monster memory -- overwriting the mimic
        // with MONS_SENSED causes travel to bounce back and
        // forth, since every time it leaves LOS of the mimic, the
        // mimic is forgotten (replaced by MONS_SENSED).
        // XXX: since mimics were changed, is this safe to remove now?
        const monster_type remembered_monster = cell.monster();
        if (remembered_monster == mon->type)
            continue;

        const monster_type mc = mon->friendly() ? MONS_SENSED_FRIENDLY
            : have_passive(passive_t::detect_montier)
            ? ash_monster_tier(mon)
            : MONS_SENSED;

        env.map_knowledge(*ri).set_detected_monster(mc);

        // Don't bother warning the player (or interrupting autoexplore) about
        // friendly monsters or those known to be easy, or those recently
        // warned about
        if (mc == MONS_SENSED_TRIVIAL || mc == MONS_SENSED_EASY
            || mc == MONS_SENSED_FRIENDLY || mon->wont_attack()
            || testbits(mon->flags, MF_SENSED))
        {
            continue;
        }

        for (radius_iterator ri2(mon->pos(), 2, C_SQUARE); ri2; ++ri2)
        {
            if (you.see_cell(*ri2))
            {
                mon->flags |= MF_SENSED;
                interrupt_activity(activity_interrupt::sense_monster);
                break;
            }
        }
    }
}

int augmentation_amount()
{
    int amount = 0;
    const int level = you.get_mutation_level(MUT_AUGMENTATION);

    for (int i = 0; i < level; ++i)
    {
        if (you.hp >= ((i + level) * you.hp_max) / (2 * level))
            amount++;
    }

    return amount;
}

void reset_powered_by_death_duration()
{
    const int pbd_dur = random_range(2, 5);
    you.set_duration(DUR_POWERED_BY_DEATH, pbd_dur);
}

static void _transpose_gear()
{
    int target_slot = EQ_FIRST_MORPH;
    for (int slot = EQ_MIN_ARMOUR; slot <= EQ_MAX_ARMOUR; slot++)
    {
        const item_def *item = you.slot_item(static_cast<equipment_type>(slot));

        if (item)
        {
            const equipment_type place = get_armour_slot(static_cast<armour_type>(item->sub_type));
            you.equip[slot] = -1;
            const bool meld = you.melded[slot];
            you.melded.set(slot, false);
            you.equip[target_slot] = item->link;
            if (meld)
                you.melded.set(target_slot, true);

            target_slot++;
            if (place == EQ_BODY_ARMOUR || place == EQ_BARDING)
                target_slot++;
        }
    }
}

static void _return_gear()
{
    for (int slot = EQ_FIRST_MORPH; slot <= EQ_LAST_MORPH; slot++)
    {
        const item_def *item = you.slot_item(static_cast<equipment_type>(slot));

        if (item)
        {
            const bool meld = you.melded[slot];
            you.equip[slot] = -1;
            you.melded.set(slot, false);

            if (!can_wear_armour(*item, false, true))
                continue;

            const equipment_type place = get_armour_slot(static_cast<armour_type>(item->sub_type));
            const item_def *conf = you.slot_item(static_cast<equipment_type>(place));
            bool swap_item = true;

            if (conf)
            {
                int keyin;
                while (true)
                {
                    if (crawl_state.seen_hups)
                        return;

                    clear_messages();

                    mprf_nojoin("[a] - %s.", conf->name(DESC_YOUR).c_str());
                    mprf_nojoin("[b] - %s.", item->name(DESC_YOUR).c_str());

                    mprf(MSGCH_PROMPT, "Continue wearing which item?");
                    keyin = toalower(get_ch()) - 'a';
                    if (keyin != 0 && keyin != 1)
                        continue;

                    break;
                }
                swap_item = keyin;
            }

            if (swap_item)
            {
                you.equip[place] = item->link;
                if (meld)
                    you.melded.set(place, true);
            }
        }
    }
}
