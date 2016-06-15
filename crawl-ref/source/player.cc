/**
 * @file
 * @brief Player related functions.
**/

#include "AppHdr.h"

#include "player.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "ability.h"
#include "abyss.h"
#include "act-iter.h"
#include "areas.h"
#include "art-enum.h"
#include "bloodspatter.h"
#include "branch.h"
#include "chardump.h"
#include "cloud.h"
#include "coordit.h"
#include "delay.h"
#include "dgn-overview.h"
#include "dgnevent.h"
#include "directn.h"
#include "english.h"
#include "env.h"
#include "errors.h"
#include "exercise.h"
#include "food.h"
#include "godabil.h"
#include "godconduct.h"
#include "godpassive.h"
#include "godwrath.h"
#include "hints.h"
#include "hiscores.h"
#include "invent.h"
#include "itemprop.h"
#include "item_use.h"
#include "kills.h"
#include "libutil.h"
#include "macro.h"
#include "melee_attack.h"
#include "message.h"
#include "misc.h"
#include "mon-place.h"
#include "mutation.h"
#include "notes.h"
#include "output.h"
#include "player-stats.h"
#include "potion.h"
#include "prompt.h"
#include "religion.h"
#include "rune_curse.h"
#include "shout.h"
#include "skills.h"
#include "spl-damage.h"
#include "spl-transloc.h"
#include "spl-util.h"
#include "sprint.h"
#include "stairs.h"
#include "stash.h"
#include "state.h"
#include "status.h"
#include "stepdown.h"
#include "stringutil.h"
#include "spl-summoning.h"
#include "terrain.h"
#ifdef USE_TILE
 #include "tiledef-feat.h"
 #include "tileview.h"
#endif
#include "transform.h"
#include "traps.h"
#include "travel.h"
#include "view.h"
#include "xom.h"
#include "place.h"
#include "items.h"
#include "decks.h"
#include "mon-pathfind.h"
#include "mon-behv.h"
#include "player-reacts.h"

static int _bone_armour_bonus();
static void _fade_curses(int exp_gained);

static void _moveto_maybe_repel_stairs()
{
    const dungeon_feature_type new_grid = env.grid(you.pos());
    const command_type stair_dir = feat_stair_direction(new_grid);

    if (stair_dir == CMD_NO_CMD
        || new_grid == DNGN_ENTER_SHOP
        ||  !you.duration[DUR_REPEL_STAIRS_MOVE])
    {
        return;
    }

    int pct = you.duration[DUR_REPEL_STAIRS_CLIMB] ? 29 : 50;

    // When the effect is still strong, the chance to actually catch
    // a stair is smaller. (Assuming the duration starts out at 1000.)
    const int dur = max(0, you.duration[DUR_REPEL_STAIRS_MOVE] - 700);
    pct += dur/10;

    if (x_chance_in_y(pct, 100))
    {
        const string stair_str = feature_description_at(you.pos(), false,
                                                        DESC_THE, false);
        const string prep = feat_preposition(new_grid, true, &you);

        if (slide_feature_over(you.pos()))
        {
            mprf("%s slides away as you move %s it!", stair_str.c_str(),
                 prep.c_str());

            if (player_in_a_dangerous_place() && one_chance_in(5))
                xom_is_stimulated(25);
        }
    }
}

static void _handle_insight(int exp_gain);

bool check_moveto_cloud(const coord_def& p, const string &move_verb,
                        bool *prompted)
{
    if (you.confused())
        return true;
    const cloud_type ctype = cloud_type_at(p);
    // Don't prompt if already in a cloud of the same type.
    if (is_damaging_cloud(ctype, true, cloud_is_yours_at(p))
        && ctype != cloud_type_at(you.pos())
        && !crawl_state.disables[DIS_CONFIRMATIONS])
    {
        // Don't prompt for steam unless we're at uncomfortably low hp.
        if (ctype == CLOUD_STEAM)
        {
            int threshold = 20;
            if (player_res_steam() < 0)
                threshold = threshold * 3 / 2;
            threshold = threshold * you.time_taken / BASELINE_DELAY;
            // Do prompt if we'd lose icemail, though.
            if (you.hp > threshold && !you.mutation[MUT_ICEMAIL])
                return true;
        }
        // Don't prompt for meph if we have clarity, unless at very low HP.
        if (ctype == CLOUD_MEPHITIC && you.clarity(false)
            && you.hp > 2 * you.time_taken / BASELINE_DELAY)
        {
            return true;
        }

        if (prompted)
            *prompted = true;
        string prompt = make_stringf("Really %s into that cloud of %s?",
                                     move_verb.c_str(),
                                     cloud_type_name(ctype).c_str());
        learned_something_new(HINT_CLOUD_WARNING);

        if (!yesno(prompt.c_str(), false, 'n'))
        {
            canned_msg(MSG_OK);
            return false;
        }
    }
    return true;
}

bool check_moveto_trap(const coord_def& p, const string &move_verb,
                       bool *prompted)
{
    // If there's no trap, let's go.
    trap_def* trap = trap_at(p);
    if (!trap || env.grid(p) == DNGN_UNDISCOVERED_TRAP)
        return true;

    if (trap->type == TRAP_ZOT && !trap->is_safe() && !crawl_state.disables[DIS_CONFIRMATIONS])
    {
        string msg = "Do you really want to %s into the Zot trap?";
        string prompt = make_stringf(msg.c_str(), move_verb.c_str());

        if (prompted)
            *prompted = true;
        if (!yes_or_no("%s", prompt.c_str()))
        {
            canned_msg(MSG_OK);
            return false;
        }
    }
    else if (!trap->is_safe() && !crawl_state.disables[DIS_CONFIRMATIONS])
    {
        string prompt;

        if (prompted)
            *prompted = true;
        prompt = make_stringf("Really %s %s that %s?", move_verb.c_str(),
                              (trap->type == TRAP_ALARM
                               || trap->type == TRAP_PLATE) ? "onto"
                              : "into",
                              feature_description_at(p, false, DESC_BASENAME,
                                                     false).c_str());
        if (!yesno(prompt.c_str(), true, 'n'))
        {
            canned_msg(MSG_OK);
            return false;
        }
    }
    return true;
}

static bool _check_moveto_dangerous(const coord_def& p, const string& msg)
{
    if (you.can_swim() && feat_is_water(env.grid(p))
        || you.airborne() || !is_feat_dangerous(env.grid(p)))
    {
        return true;
    }

    if (msg != "")
        mpr(msg);
    else if (species_likes_water(you.species) && feat_is_water(env.grid(p)))
        mpr("You cannot enter water in your current form.");
    else if (species_likes_lava(you.species) && feat_is_lava(env.grid(p)))
        mpr("You cannot enter lava in your current form.");
    else
        canned_msg(MSG_UNTHINKING_ACT);
    return false;
}

bool check_moveto_terrain(const coord_def& p, const string &move_verb,
                          const string &msg, bool *prompted)
{
    if (!_check_moveto_dangerous(p, msg))
        return false
;
    if (!need_expiration_warning() && need_expiration_warning(p)
        && !crawl_state.disables[DIS_CONFIRMATIONS])
    {
        string prompt;

        if (prompted)
            *prompted = true;

        if (msg != "")
            prompt = msg + " ";

        prompt += "Are you sure you want to " + move_verb;

        if (you.ground_level())
            prompt += " into ";
        else
            prompt += " over ";

        prompt += env.grid(p) == DNGN_DEEP_WATER ? "deep water" : "lava";

        prompt += need_expiration_warning(DUR_FLIGHT, p)
            ? " while you are losing your buoyancy?"
            : " while your transformation is expiring?";

        if (!yesno(prompt.c_str(), false, 'n'))
        {
            canned_msg(MSG_OK);
            return false;
        }
    }
    return true;
}

bool check_moveto_exclusion(const coord_def& p, const string &move_verb,
                            bool *prompted)
{
    string prompt;

    if (is_excluded(p)
        && !is_stair_exclusion(p)
        && !is_excluded(you.pos())
        && !crawl_state.disables[DIS_CONFIRMATIONS])
    {
        if (prompted)
            *prompted = true;
        prompt = make_stringf("Really %s into a travel-excluded area?",
                              move_verb.c_str());

        if (!yesno(prompt.c_str(), false, 'n'))
        {
            canned_msg(MSG_OK);
            return false;
        }
    }
    return true;
}

bool check_moveto(const coord_def& p, const string &move_verb, const string &msg)
{
    return check_moveto_terrain(p, move_verb, msg)
           && check_moveto_cloud(p, move_verb)
           && check_moveto_trap(p, move_verb)
           && check_moveto_exclusion(p, move_verb);
}

// Returns true if this is a valid swap for this monster. If true, then
// the valid location is set in loc. (Otherwise loc becomes garbage.)
bool swap_check(monster* mons, coord_def &loc, bool quiet)
{
    loc = you.pos();

    if (you.is_stationary())
        return false;

    // Don't move onto dangerous terrain.
    if (is_feat_dangerous(grd(mons->pos())))
    {
        canned_msg(MSG_UNTHINKING_ACT);
        return false;
    }

    if (mons->is_projectile())
    {
        if (!quiet)
            mpr("It's unwise to walk into this.");
        return false;
    }

    if (mons->caught())
    {
        if (!quiet)
        {
            simple_monster_message(mons,
                make_stringf(" is %s!", held_status(mons)).c_str());
        }
        return false;
    }

    if (mons->is_constricted())
    {
        if (!quiet)
            simple_monster_message(mons, " is being constricted!");
        return false;
    }

    if (mons->is_stationary() || mons->asleep() || mons->cannot_move())
    {
        if (!quiet)
            simple_monster_message(mons, " cannot move out of your way!");
        return false;
    }

    // prompt when swapping into known zot traps
    if (!quiet && trap_at(loc) && trap_at(loc)->type == TRAP_ZOT
        && env.grid(loc) != DNGN_UNDISCOVERED_TRAP
        && !yes_or_no("Do you really want to swap %s into the Zot trap?",
                      mons->name(DESC_YOUR).c_str()))
    {
        return false;
    }

    // First try: move monster onto your position.
    bool swap = !monster_at(loc) && monster_habitable_grid(mons, grd(loc));

    if (monster_at(loc)
        && monster_at(loc)->type == MONS_TOADSTOOL
        && mons->type == MONS_WANDERING_MUSHROOM)
    {
        swap = monster_habitable_grid(mons, grd(loc));
    }

    // Choose an appropriate habitat square at random around the target.
    if (!swap)
    {
        int num_found = 0;

        for (adjacent_iterator ai(mons->pos()); ai; ++ai)
            if (!monster_at(*ai) && monster_habitable_grid(mons, grd(*ai))
                && one_chance_in(++num_found))
            {
                loc = *ai;
            }

        if (num_found)
            swap = true;
    }

    if (!swap && !quiet)
    {
        // Might not be ideal, but it's better than insta-killing
        // the monster... maybe try for a short blink instead? - bwr
        simple_monster_message(mons, " cannot make way for you.");
        // FIXME: AI_HIT_MONSTER isn't ideal.
        interrupt_activity(AI_HIT_MONSTER, mons);
    }

    return swap;
}

static void _splash()
{
    if (you.can_swim())
        noisy(4, you.pos(), "Floosh!");
    else if (!you.can_water_walk())
        noisy(8, you.pos(), "Splash!");
}

void moveto_location_effects(dungeon_feature_type old_feat,
                             bool stepped, const coord_def& old_pos)
{
    const dungeon_feature_type new_grid = env.grid(you.pos());

    // Terrain effects.
    if (is_feat_dangerous(new_grid))
        fall_into_a_pool(new_grid);

    // called after fall_into_a_pool, in case of emergency untransform
    if (you.species == SP_MERFOLK)
        merfolk_check_swimming(stepped);

    if (you.ground_level())
    {
        if (player_likes_lava(false))
        {
            if (feat_is_lava(new_grid) && !feat_is_lava(old_feat))
            {
                if (!stepped)
                    noisy(4, you.pos(), "Gloop!");

                mprf("You %s lava.",
                     (stepped) ? "slowly immerse yourself in the" : "fall into the");

                // Extra time if you stepped in.
                if (stepped)
                    you.time_taken *= 2;
                // This gets called here because otherwise you wouldn't heat
                // until your second turn in lava.
                if (temperature() < TEMP_FIRE)
                    mpr("The lava instantly superheats you.");
                you.temperature = TEMP_MAX;
            }

            else if (!feat_is_lava(new_grid) && feat_is_lava(old_feat))
            {
                mpr("You slowly pull yourself out of the lava.");
                you.time_taken *= 2;
            }
        }

        if (feat_is_water(new_grid))
        {
            if (!stepped)
                _splash();

            if (!you.can_swim() && !you.can_water_walk())
            {
                if (stepped)
                {
                    you.time_taken *= 13 + random2(8);
                    you.time_taken /= 10;
                }

                if (!feat_is_water(old_feat))
                {
                    mprf("You %s the %s water.",
                         stepped ? "enter" : "fall into",
                         new_grid == DNGN_SHALLOW_WATER ? "shallow" : "deep");
                }

                if (new_grid == DNGN_DEEP_WATER && old_feat != DNGN_DEEP_WATER)
                    mpr("You sink to the bottom.");

                if (!feat_is_water(old_feat))
                {
                    mpr("Moving in this stuff is going to be slow.");
                    if (you.invisible())
                        mpr("...and don't expect to remain undetected.");
                }
            }
        }
        else if (you.props.exists(TEMP_WATERWALK_KEY))
            you.props.erase(TEMP_WATERWALK_KEY);
    }

    // Traps go off.
    // (But not when losing flight - i.e., moving into the same tile)
    trap_def* ptrap = trap_at(you.pos());
    if (ptrap && old_pos != you.pos())
        ptrap->trigger(you, !stepped); // blinking makes it hard to evade

    if (stepped)
        _moveto_maybe_repel_stairs();
}

// Use this function whenever the player enters (or lands and thus re-enters)
// a grid.
//
// stepped     - normal walking moves
void move_player_to_grid(const coord_def& p, bool stepped)
{
    ASSERT(!crawl_state.game_is_arena());
    ASSERT_IN_BOUNDS(p);

    if (!stepped)
        tornado_move(p);

    // assuming that entering the same square means coming from above (flight)
    const coord_def old_pos = you.pos();
    const bool from_above = (old_pos == p);
    const dungeon_feature_type old_grid =
        (from_above) ? DNGN_FLOOR : grd(old_pos);

    // Really must be clear.
    ASSERT(you.can_pass_through_feat(grd(p)));

    // Better not be an unsubmerged monster either.
    ASSERT(!monster_at(p) || monster_at(p)->submerged()
           || fedhas_passthrough(monster_at(p))
           || mons_is_player_shadow(monster_at(p)));

    // Move the player to new location.
    you.moveto(p, true);
    viewwindow();

    moveto_location_effects(old_grid, stepped, old_pos);
}


/**
 * Check if the given terrain feature is safe for the player to move into.
 * (Or, at least, not instantly lethal.)
 *
 * @param grid          The type of terrain feature under consideration.
 * @param permanently   Whether to disregard temporary effects (non-permanent
 *                      flight, forms, etc)
 * @param ignore_flight Whether to ignore all forms of flight (including
 *                      permanent flight)
 * @return              Whether the terrain is safe.
 */
bool is_feat_dangerous(dungeon_feature_type grid, bool permanently,
                       bool ignore_flight)
{
    if (!ignore_flight
        && (you.permanent_flight() || you.airborne() && !permanently))
    {
        return false;
    }
    else if (grid == DNGN_DEEP_WATER && !player_likes_water(permanently)
             || grid == DNGN_LAVA && !player_likes_lava(permanently))
    {
        return true;
    }
    else
        return false;
}

bool is_map_persistent()
{
    return !(your_branch().branch_flags & BFLAG_NO_MAP)
           || env.properties.exists(FORCE_MAPPABLE_KEY);
}

bool player_in_hell()
{
    return is_hell_subbranch(you.where_are_you);
}

/**
 * Is the player in the slightly-special version of the abyss that AKs start
 * in?
 */
bool player_in_starting_abyss()
{
    return you.chapter == CHAPTER_POCKET_ABYSS
           && player_in_branch(BRANCH_ABYSS) && you.depth <= 1;
}

bool player_in_connected_branch()
{
    return is_connected_branch(you.where_are_you);
}

bool player_likes_water(bool permanently)
{
    return !permanently && you.can_water_walk()
           || (species_likes_water(you.species) || !permanently)
               && form_likes_water();
}

bool player_likes_lava(bool permanently)
{
    return (species_likes_lava(you.species) || !permanently)
           && form_likes_lava();
}

bool player_can_open_doors()
{
    return you.form != TRAN_BAT;
}

// If transform is true, compare with current transformation instead
// of (or in addition to) underlying species.
// (See mon-data.h for species/genus use.)
bool is_player_same_genus(const monster_type mon)
{
    // Genus would include necrophage and rotting hulk.
    if (you.species == SP_GHOUL)
        return mons_species(mon) == MONS_GHOUL;

    // Note that these are currently considered to be the same genus:
    // * humans, demigods, and demonspawn
    // * ogres and two-headed ogres
    // * trolls, iron trolls, and deep trolls
    // * kobolds and big kobolds
    // * dwarves and deep dwarves
    // * all elf races
    // * all orc races
    return mons_genus(mon) == mons_genus(player_mons(false));
}

void update_player_symbol()
{
    you.symbol = Options.show_player_species ? player_mons() : transform_mons();
}

monster_type player_mons(bool transform)
{
    monster_type mons;

    if (transform)
    {
        mons = transform_mons();
        if (mons != MONS_PLAYER)
            return mons;
    }

    mons = player_species_to_mons_species(you.species);

    if (mons == MONS_ORC)
    {
        if (you_worship(GOD_BEOGH))
        {
            mons = (you.piety >= piety_breakpoint(4)) ? MONS_ORC_HIGH_PRIEST
                                                      : MONS_ORC_PRIEST;
        }
    }
    else if (mons == MONS_OGRE)
    {
        const skill_type sk = best_skill(SK_FIRST_SKILL, SK_LAST_SKILL);
        if (sk >= SK_SPELLCASTING && sk < SK_INVOCATIONS)
            mons = MONS_OGRE_MAGE;
    }

    return mons;
}

void update_vision_range()
{
    you.normal_vision = LOS_RADIUS;
    int nom   = 1;
    int denom = 1;

    if (player_mutation_level(MUT_GLOW) > 2)
    {
    	nom *= 6, denom *= 4;
    }

    // Nightstalker gives -1/-2/-3.
    if (player_mutation_level(MUT_NIGHTSTALKER))
    {
        nom *= LOS_RADIUS - player_mutation_level(MUT_NIGHTSTALKER);
        denom *= LOS_RADIUS;
    }

    // Lantern of shadows.
    if (you.attribute[ATTR_SHADOWS])
        nom *= 3, denom *= 4;

    // the Darkness spell.
    if (you.duration[DUR_DARKNESS])
        nom *= 3, denom *= 4;

    // robe of Night.
    if (player_equip_unrand(UNRAND_NIGHT))
        nom *= 3, denom *= 4;

    you.current_vision = min(LOS_RADIUS, (you.normal_vision * nom + denom / 2) / denom);
    ASSERT(you.current_vision > 0);
    set_los_radius(you.current_vision);
}

/**
 * Ignoring form & most equipment, but not the UNRAND_FINGER_AMULET, can the
 * player use (usually wear) a given equipment slot?
 *
 * @param eq   The slot in question.
 * @param temp Whether to consider forms.
 * @return   MB_FALSE if the player can never use the slot;
 *           MB_MAYBE if the player can only use some items for the slot;
 *           MB_TRUE  if the player can use any (fsvo any) item for the slot.
 */
maybe_bool you_can_wear(equipment_type eq, bool temp)
{
    if (temp && !get_form()->slot_available(eq))
        return MB_FALSE;

    switch (eq)
    {
    case EQ_LEFT_RING:
        if (you.species == SP_TENGU || player_mutation_level(MUT_MISSING_HAND))
            return MB_FALSE;
        // intentional fallthrough
    case EQ_RIGHT_RING:
        return you.species != SP_OCTOPODE && you.species != SP_FELID && you.species != SP_TENGU ? MB_TRUE : MB_FALSE;

    case EQ_RING_EIGHT:
        if (player_mutation_level(MUT_MISSING_HAND))
            return MB_FALSE;
        return you.species == SP_OCTOPODE ? MB_TRUE : MB_FALSE;
    case EQ_RING_ONE:
    case EQ_RING_TWO:
    case EQ_RING_THREE:
    case EQ_RING_FOUR:
        return you.species == SP_OCTOPODE || you.species == SP_FELID ? MB_TRUE : MB_FALSE;
    case EQ_RING_FIVE:
    case EQ_RING_SIX:
    case EQ_RING_SEVEN:
        return you.species == SP_OCTOPODE ? MB_TRUE : MB_FALSE;

    case EQ_WEAPON:
    case EQ_STAFF:
        return you.species == SP_FELID ? MB_FALSE :
               you.body_size(PSIZE_TORSO, !temp) < SIZE_MEDIUM ? MB_MAYBE :
                                         MB_TRUE;

    // You can always wear at least one ring (forms were already handled).
    case EQ_RINGS:
    	if (you.species == SP_TENGU)
    		return MB_FALSE;
    case EQ_ALL_ARMOUR:
    case EQ_AMULET:
        return MB_TRUE;

    case EQ_RING_AMULET:
        return player_equip_unrand(UNRAND_FINGER_AMULET) ? MB_TRUE : MB_FALSE;

    default:
        break;
    }

    item_def dummy, alternate;
    dummy.base_type = alternate.base_type = OBJ_ARMOUR;
    dummy.sub_type = alternate.sub_type = NUM_ARMOURS;
    // Make sure can_wear_armour doesn't think it's Lear's.
    dummy.unrand_idx = alternate.unrand_idx = 0;

    switch (eq)
    {
    case EQ_CLOAK:
        dummy.sub_type = ARM_CLOAK;
        break;

    case EQ_GLOVES:
        dummy.sub_type = ARM_GLOVES;
        break;

    case EQ_BOOTS: // And bardings
        dummy.sub_type = ARM_BOOTS;
        if (you.species == SP_NAGA)
            alternate.sub_type = ARM_NAGA_BARDING;
        if (you.species == SP_CENTAUR)
            alternate.sub_type = ARM_CENTAUR_BARDING;
        break;

    case EQ_BODY_ARMOUR:
        // Assume that anything that can wear any armour at all can wear a robe
        // and that anything that can wear CPA can wear all armour.
        dummy.sub_type = ARM_CRYSTAL_PLATE_ARMOUR;
        alternate.sub_type = ARM_ROBE;
        break;

    case EQ_SHIELD:
        // No races right now that can wear ARM_LARGE_SHIELD but not ARM_SHIELD
        dummy.sub_type = ARM_LARGE_SHIELD;
        if (you.body_size(PSIZE_TORSO, !temp) < SIZE_MEDIUM)
            alternate.sub_type = ARM_BUCKLER;
        break;

    case EQ_HELMET:
        dummy.sub_type = ARM_HELMET;
        alternate.sub_type = ARM_HAT;
        break;

    default:
        die("unhandled equipment type %d", eq);
        break;
    }

    ASSERT(dummy.sub_type != NUM_ARMOURS);

    if (can_wear_armour(dummy, false, !temp))
        return MB_TRUE;
    else if (alternate.sub_type != NUM_ARMOURS
             && can_wear_armour(alternate, false, !temp))
    {
        return MB_MAYBE;
    }
    else
        return MB_FALSE;
}

bool player_has_feet(bool temp)
{
    if (you.species == SP_NAGA
        || you.species == SP_FELID
        || you.species == SP_OCTOPODE
        || you.species == SP_DJINNI
        || you.fishtail && temp)
    {
        return false;
    }

    if (player_mutation_level(MUT_HOOVES, temp) == 3
        || player_mutation_level(MUT_TALONS, temp) == 3)
    {
        return false;
    }

    return true;
}

// Returns false if the player is wielding a weapon inappropriate for Berserk.
bool berserk_check_wielded_weapon()
{
    const item_def * const wpn = you.weapon();
    bool penance = false;
    if (wpn && wpn->defined()
        && (!is_melee_weapon(*wpn)
            || needs_handle_warning(*wpn, OPER_ATTACK, penance)))
    {
        string prompt = "Do you really want to go berserk while wielding "
                        + wpn->name(DESC_YOUR) + "?";
        if (penance)
            prompt += " This could place you under penance!";

        if (!yesno(prompt.c_str(), true, 'n'))
        {
            canned_msg(MSG_OK);
            return false;
        }
    }

    return true;
}

// Looks in equipment "slot" to see if there is an equipped "sub_type".
// Returns number of matches (in the case of rings, both are checked)
int player::wearing(equipment_type slot, int sub_type, bool calc_unid) const
{
    int ret = 0;

    const item_def* item;

    switch (slot)
    {
        case EQ_WEAPON:
        {
            item = weapon();
            // Hands can have more than just weapons.
            if (item && item->is_type(OBJ_WEAPONS, sub_type) && !item->super_cursed())
                ret++;
            break;
        }

        case EQ_STAFF:
        {
            item = weapon();
            // Like above, but must be magical staff.
            if (item
                && item->is_type(OBJ_STAVES, sub_type)
                && (calc_unid || item_type_known(*item))
                && !item->super_cursed()
                )
            {
                ret++;
            }
            break;
        }

        case EQ_AMULET:
        case EQ_AMULET_PLUS:
            if ((item = slot_item(static_cast<equipment_type>(EQ_AMULET)))
                && item->sub_type == sub_type
                && (calc_unid
                    || item_type_known(*item))
                && !item->super_cursed()
                )
            {
                ret += (slot == EQ_AMULET_PLUS ? item->plus : 1);
            }
            break;

        case EQ_RINGS:
        case EQ_RINGS_PLUS:
            for (int slots = EQ_LEFT_RING; slots < NUM_EQUIP; slots++)
            {
                if (slots == EQ_AMULET)
                    continue;

                if ((item = slot_item(static_cast<equipment_type>(slots)))
                    && item->sub_type == sub_type
                    && (calc_unid
                        || item_type_known(*item)))
                {
                    int bonus = 1;
                    if (slot == EQ_RINGS_PLUS)
                        bonus = item->plus;

                    if (item->super_cursed())
                        bonus = 0;
                    ret += bonus;
                }
            }
            break;

        case EQ_ALL_ARMOUR:
            // Doesn't make much sense here... be specific. -- bwr
            die("EQ_ALL_ARMOUR is not a proper slot");
            break;

        default:
            if (! (slot > EQ_NONE && slot < NUM_EQUIP))
                die("invalid slot");
            if ((item = slot_item(slot))
                && item->sub_type == sub_type
                && (calc_unid || item_type_known(*item))
                && !item->super_cursed()
                )
            {
                ret++;
            }
            break;
    }

    return ret;
}

// Looks in equipment "slot" to see if equipped item has "special" ego-type
// Returns number of matches (jewellery returns zero -- no ego type).
// [ds] There's no equivalent of calc_unid or req_id because as of now, weapons
// and armour type-id on wield/wear.
int player::wearing_ego(equipment_type slot, int special, bool calc_unid) const
{
    int ret = 0;

    const item_def* item;
    switch (slot)
    {
        case EQ_WEAPON:
            // Hands can have more than just weapons.
            if ((item = slot_item(EQ_WEAPON))
                && item->base_type == OBJ_WEAPONS
                && get_weapon_brand(*item) == special
                && !item->super_cursed()
                )
            {
                ret++;
            }
            break;

        case EQ_LEFT_RING:
        case EQ_RIGHT_RING:
        case EQ_AMULET:
        case EQ_STAFF:
        case EQ_RINGS:
        case EQ_RINGS_PLUS:
            // no ego types for these slots
            break;

        case EQ_ALL_ARMOUR:
            // Check all armour slots:
            for (int i = EQ_MIN_ARMOUR; i <= EQ_MAX_ARMOUR; i++)
            {
                if ((item = slot_item(static_cast<equipment_type>(i)))
                    && get_armour_ego_type(*item) == special
                    && (calc_unid || item_type_known(*item))
                    && !item->super_cursed()
                    )
                {
                    ret++;
                }
            }
            break;

        default:
            if (slot < EQ_MIN_ARMOUR || slot > EQ_MAX_ARMOUR)
                die("invalid slot: %d", slot);
            // Check a specific armour slot for an ego type:
            if ((item = slot_item(static_cast<equipment_type>(slot)))
                && get_armour_ego_type(*item) == special
                && (calc_unid || item_type_known(*item))
                && !item->super_cursed()
                )
            {
                ret++;
            }
            break;
    }

    return ret;
}

// Returns true if the indicated unrandart is equipped
// [ds] There's no equivalent of calc_unid or req_id because as of now, weapons
// and armour type-id on wield/wear.
bool player_equip_unrand(int unrand_index)
{
    const unrandart_entry* entry = get_unrand_entry(unrand_index);
    equipment_type   slot  = get_item_slot(entry->base_type,
                                           entry->sub_type);

    item_def* item;

    switch (slot)
    {
        case EQ_WEAPON:
            // Hands can have more than just weapons.
            if ((item = you.slot_item(slot))
                && item->base_type == OBJ_WEAPONS
                && is_unrandom_artefact(*item)
                && item->unrand_idx == unrand_index)
            {
                return true;
            }
            break;

        case EQ_RINGS:
            for (int slots = EQ_LEFT_RING; slots < NUM_EQUIP; ++slots)
            {
                if (slots == EQ_AMULET)
                    continue;

                if ((item = you.slot_item(static_cast<equipment_type>(slots)))
                    && is_unrandom_artefact(*item)
                    && item->unrand_idx == unrand_index)
                {
                    return true;
                }
            }
            break;

        case EQ_NONE:
        case EQ_STAFF:
        case EQ_LEFT_RING:
        case EQ_RIGHT_RING:
        case EQ_RINGS_PLUS:
        case EQ_ALL_ARMOUR:
            // no unrandarts for these slots.
            break;

        default:
            if (slot <= EQ_NONE || slot >= NUM_EQUIP)
                die("invalid slot: %d", slot);
            // Check a specific slot.
            if ((item = you.slot_item(slot))
                && is_unrandom_artefact(*item)
                && item->unrand_idx == unrand_index)
            {
                return true;
            }
            break;
    }

    return false;
}

bool player_can_hear(const coord_def& p, int hear_distance)
{
    return !silenced(p)
           && !silenced(you.pos())
           && you.pos().distance_from(p) <= hear_distance;
}

int player_teleport(bool calc_unid)
{
    ASSERT(!crawl_state.game_is_arena());

    // Don't allow any form of teleportation in Sprint.
    if (crawl_state.game_is_sprint())
        return 0;

    // Short-circuit rings of teleport to prevent spam.
    if (you.species == SP_FORMICID)
        return 0;

    int tp = 0;

    // rings (keep in sync with _equip_jewellery_effect)
    tp += 8 * you.wearing(EQ_RINGS, RING_TELEPORTATION, calc_unid);

    // artefacts
    tp += 8 * you.scan_artefacts(ARTP_CAUSE_TELEPORTATION, calc_unid);

    // mutations
    tp += player_mutation_level(MUT_TELEPORT) * 4;

    return tp;
}

// Computes bonuses to regeneration from most sources. Does not handle
// slow regeneration, vampireness, or Trog's Hand.
static int _player_bonus_regen()
{
    int rr = 0;

    // Trog's Hand is handled separately so that it will bypass slow
    // regeneration, and it overrides the spell.
    if (you.duration[DUR_REGENERATION]
        && !you.duration[DUR_TROGS_HAND])
    {
        rr += 100;
    }

    // Jewellery.
    if (you.props[REGEN_AMULET_ACTIVE].get_int() == 1)
        rr += REGEN_PIP * you.wearing(EQ_AMULET, AMU_HEALTH_REGENERATION);

    // Artefacts
    rr += REGEN_PIP * you.scan_artefacts(ARTP_REGENERATION);

    // Troll leather
    if (you.wearing(EQ_BODY_ARMOUR, ARM_TROLL_LEATHER_ARMOUR)
        || you.wearing(EQ_BODY_ARMOUR, ARM_TROLL_HIDE))
    {
        rr += REGEN_PIP;
    }

    // Fast heal mutation.
    rr += player_mutation_level(MUT_HEALTH_REGENERATION) * 20;

    // Powered By Death mutation, boosts regen by variable strength
    // if the duration of the effect is still active.
    if (you.duration[DUR_POWERED_BY_DEATH])
        rr += you.props[POWERED_BY_DEATH_KEY].get_int() * 100;

    return rr;
}

// Slow regeneration mutation: slows or stops regeneration when monsters are
// visible at level 1 or 2 respectively, stops regeneration at level 3.
static int _slow_regeneration_rate()
{
    if (player_mutation_level(MUT_SLOW_REGENERATION) == 3)
        return 0;

    for (monster_near_iterator mi(you.pos(), LOS_NO_TRANS); mi; ++mi)
    {
        if (mons_is_threatening(*mi)
            && !mi->wont_attack()
            && !mi->neutral())
        {
            return 2 - player_mutation_level(MUT_SLOW_REGENERATION);
        }
    }
    return 2;
}

int player_regen()
{
    // Note: if some condition can set rr = 0, can't be rested off, and
    // would allow travel, please update is_sufficiently_rested.

    int rr = you.hp_max / 3;

    // Add in miscellaneous bonuses
    rr += _player_bonus_regen();

    // Before applying other effects, make sure that there's something
    // to heal.
    rr = max(1, rr);

    // Healing depending on satiation.
    // The better-fed you are, the faster you heal.
    if (you.species == SP_VAMPIRE)
    {
        if (you.hunger_state <= HS_STARVING)
            rr = 0;   // No regeneration for starving vampires.
        else if (you.hunger_state == HS_ENGORGED)
            rr += 20; // More bonus regeneration for engorged vampires.
        else if (you.hunger_state < HS_SATIATED)
            rr /= 2;  // Halved regeneration for hungry vampires.
        else if (you.hunger_state >= HS_FULL)
            rr += 10; // Bonus regeneration for full vampires.
    }

    // Slow regeneration mutation.
    if (player_mutation_level(MUT_SLOW_REGENERATION) > 0)
    {
        rr *= _slow_regeneration_rate();
        rr /= 2;
    }
    if (you.duration[DUR_COLLAPSE])
        rr /= 4;

    if (you.disease)
        rr = 0;

    // Trog's Hand. This circumvents the slow regeneration mutation.
    if (you.duration[DUR_TROGS_HAND])
    {
        rr += 100;
        if(you.species == SP_DEEP_DWARF)
        {
            rr <<= 1;
        }
    }

    return rr;
}

// Amulet of regeneration needs to be worn while at full health before it begins
// to function.
void update_regen_amulet_attunement()
{
    if (you.wearing(EQ_AMULET, AMU_HEALTH_REGENERATION)
        && player_mutation_level(MUT_SLOW_REGENERATION) < 3)
    {
        if (you.hp == you.hp_max
            && you.props[REGEN_AMULET_ACTIVE].get_int() == 0)
        {
            you.props[REGEN_AMULET_ACTIVE] = 1;
            mpr("Your amulet attunes itself to your body and you begin to "
                    "regenerate more quickly.");
        }
    }
    else
        you.props[REGEN_AMULET_ACTIVE] = 0;
}

// Amulet of magic regeneration needs to be worn while at full magic before it
// begins to function.
void update_magic_regen_amulet_attunement()
{
    if (you.wearing(EQ_AMULET, AMU_MAGIC_REGENERATION)
        && player_regenerates_mp())
    {
        if (you.mp == you.mp_max
            && you.props[MANA_REGEN_AMULET_ACTIVE].get_int() == 0)
        {
            you.props[MANA_REGEN_AMULET_ACTIVE] = 1;
            mpr("Your amulet attunes itself to your body and you begin to "
                    "regenerate magic more quickly.");
        }
    }
    else
        you.props[MANA_REGEN_AMULET_ACTIVE] = 0;
}

int player_hunger_rate(bool temp)
{
    int hunger = 3;

    if (you.species != SP_VAMPIRE)
        hunger = 0;

    if (temp && you.form == TRAN_BAT && you.species == SP_VAMPIRE)
        return 1;

    if (temp
        && (you.duration[DUR_REGENERATION]
            || you.duration[DUR_TROGS_HAND])
        && you.hp < you.hp_max)
    {
        hunger += 4;
    }

    /*
    if (temp)
    {
        if (you.duration[DUR_INVIS] && you.duration_source[DUR_INVIS] != SRC_POTION)
        {
            hunger += 50;
        }

        // Berserk has its own food penalty - excluding berserk haste.
        // Doubling the hunger cost for haste so that the per turn hunger
        // is consistent now that a hasted turn causes 50% the normal hunger
        // -cao
        if (you.duration[DUR_HASTE] && you.duration_source[DUR_HASTE] != SRC_POTION)
        {
            hunger += haste_mul(50);
        }
    }
     */

    if (you.species == SP_VAMPIRE)
    {
        switch (you.hunger_state)
        {
            case HS_FAINTING:
            case HS_STARVING:
                hunger -= 3;
                break;
            case HS_NEAR_STARVING:
            case HS_VERY_HUNGRY:
                hunger -= 2;
                break;
            case HS_HUNGRY:
                hunger--;
                break;
            case HS_SATIATED:
                break;
            case HS_FULL:
                hunger++;
                break;
            case HS_VERY_FULL:
                hunger += 2;
                break;
            case HS_ENGORGED:
                hunger += 3;
        }
    }

    // If Cheibriados has slowed your life processes, you will hunger less.
    if (have_passive(passive_t::slow_metabolism))
        hunger /= 2;

    if (hunger < 0)
        hunger = 0;

    return hunger;
}

int player_spell_levels()
{
    int sl = effective_xl() - 1 + you.skill(SK_SPELLCASTING, 2, true);

    bool fireball = false;
    bool delayed_fireball = false;

    if (sl > 99)
        sl = 99;

    for (const spell_type spell : you.spells)
    {
        if (spell == SPELL_FIREBALL)
            fireball = true;
        else if (spell == SPELL_DELAYED_FIREBALL)
            delayed_fireball = true;

        if (spell != SPELL_NO_SPELL)
            sl -= spell_difficulty(spell);
    }

    // Fireball is free for characters with delayed fireball
    if (fireball && delayed_fireball)
        sl += spell_difficulty(SPELL_FIREBALL);

    // moon troll has very poor spellcasting, so needs a spell slot boost to help at the beginning
    if (you.species == SP_MOON_TROLL)
        sl += 3;

    // Note: This can happen because of level drain. Maybe we should
    // force random spells out when that happens. -- bwr
    if (sl < 0)
        sl = 0;

    return sl;
}

int player_likes_chunks(bool permanently)
{
    return you.gourmand(true, !permanently)
           ? 3 : player_mutation_level(MUT_CARNIVOROUS);
}

// If temp is set to false, temporary sources or resistance won't be counted.
int player_res_fire(bool calc_unid, bool temp, bool items)
{
    int rf = 0;

    if (items)
    {
        // rings of fire resistance/fire
        rf += you.wearing(EQ_RINGS, RING_PROTECTION_FROM_FIRE, calc_unid);
        rf += you.wearing(EQ_RINGS, RING_FIRE, calc_unid);

        // rings of ice
        rf -= you.wearing(EQ_RINGS, RING_ICE, calc_unid);

        // Staves
        rf += you.wearing(EQ_STAFF, STAFF_FIRE, calc_unid);

        // body armour:
        const item_def *body_armour = you.slot_item(EQ_BODY_ARMOUR);
        if (body_armour)
            rf += armour_type_prop(body_armour->sub_type, ARMF_RES_FIRE);

        // ego armours
        rf += you.wearing_ego(EQ_ALL_ARMOUR, SPARM_FIRE_RESISTANCE);
        rf += you.wearing_ego(EQ_ALL_ARMOUR, SPARM_RESISTANCE);

        // randart weapons:
        rf += you.scan_artefacts(ARTP_FIRE, calc_unid);

        // dragonskin cloak: 0.5 to draconic resistances
        if (calc_unid && player_equip_unrand(UNRAND_DRAGONSKIN)
            && coinflip())
        {
            rf++;
        }
    }

    // species:
    if (you.species == SP_MUMMY)
        rf--;

    if (you.species == SP_LAVA_ORC)
    {
        if (temperature_effect(LORC_FIRE_RES_I))
            rf++;
        if (temperature_effect(LORC_FIRE_RES_II))
            rf++;
        if (temperature_effect(LORC_FIRE_RES_III))
            rf++;
    }

    // mutations:
    rf += player_mutation_level(MUT_HEAT_RESISTANCE, temp);
    rf -= player_mutation_level(MUT_HEAT_VULNERABILITY, temp);
    rf -= player_mutation_level(MUT_TEMPERATURE_SENSITIVITY, temp);
    rf += player_mutation_level(MUT_MOLTEN_SCALES, temp) == 3 ? 1 : 0;

    // spells:
    if (temp)
    {
        if (you.duration[DUR_RESISTANCE])
            rf++;

        if (you.duration[DUR_FIRE_SHIELD])
            rf += 2;

        if (you.duration[DUR_QAZLAL_FIRE_RES])
            rf++;

        rf += get_form()->res_fire();
    }

    if (rf > 3)
        rf = 3;
    if (temp && you.duration[DUR_FIRE_VULN])
        rf--;
    if (rf < -3)
        rf = -3;

    return rf;
}

int player_res_steam(bool calc_unid, bool temp, bool items)
{
    int res = 0;
    const int rf = player_res_fire(calc_unid, temp, items);

    if (you.species == SP_PALE_DRACONIAN)
        res += 2;

    if (items)
    {
        const item_def *body_armour = you.slot_item(EQ_BODY_ARMOUR);
        if (body_armour)
            res += armour_type_prop(body_armour->sub_type, ARMF_RES_STEAM) * 2;
    }

    res += rf * 2;

    if (res > 2)
        res = 2;

    return res;
}

int player_res_cold(bool calc_unid, bool temp, bool items)
{
    int rc = 0;

    if (temp)
    {
        if (you.duration[DUR_RESISTANCE])
            rc++;

        if (you.duration[DUR_FIRE_SHIELD])
            rc -= 2;

        if (you.duration[DUR_QAZLAL_COLD_RES])
            rc++;

        rc += get_form()->res_cold();

        if (you.species == SP_VAMPIRE)
        {
            if (you.hunger_state <= HS_NEAR_STARVING)
                rc += 2;
            else if (you.hunger_state < HS_SATIATED)
                rc++;
        }

        if (you.species == SP_LAVA_ORC && temperature_effect(LORC_COLD_VULN))
            rc--;
    }

    if (items)
    {
        // rings of cold resistance/ice
        rc += you.wearing(EQ_RINGS, RING_PROTECTION_FROM_COLD, calc_unid);
        rc += you.wearing(EQ_RINGS, RING_ICE, calc_unid);

        // rings of fire
        rc -= you.wearing(EQ_RINGS, RING_FIRE, calc_unid);

        // Staves
        rc += you.wearing(EQ_STAFF, STAFF_COLD, calc_unid);

        // body armour:
        const item_def *body_armour = you.slot_item(EQ_BODY_ARMOUR);
        if (body_armour && !body_armour->super_cursed())
            rc += armour_type_prop(body_armour->sub_type, ARMF_RES_COLD);

        // ego armours
        rc += you.wearing_ego(EQ_ALL_ARMOUR, SPARM_COLD_RESISTANCE);
        rc += you.wearing_ego(EQ_ALL_ARMOUR, SPARM_RESISTANCE);

        // randart weapons:
        rc += you.scan_artefacts(ARTP_COLD, calc_unid);

        // dragonskin cloak: 0.5 to draconic resistances
        if (calc_unid && player_equip_unrand(UNRAND_DRAGONSKIN) && coinflip())
            rc++;
    }

    // mutations:
    rc += player_mutation_level(MUT_COLD_RESISTANCE, temp);
    rc -= player_mutation_level(MUT_COLD_VULNERABILITY, temp);
    rc -= player_mutation_level(MUT_TEMPERATURE_SENSITIVITY, temp);
    rc += player_mutation_level(MUT_ICY_BLUE_SCALES, temp) == 3 ? 1 : 0;
    rc += player_mutation_level(MUT_SHAGGY_FUR, temp) == 3 ? 1 : 0;

    if (rc < -3)
        rc = -3;
    else if (rc > 3)
        rc = 3;

    return rc;
}

bool player::res_corr(bool calc_unid, bool items) const
{
    if (have_passive(passive_t::resist_corrosion))
        return true;

    if (get_form()->res_acid())
        return true;

    if (you.duration[DUR_RESISTANCE])
        return true;

    if ((form_keeps_mutations() || form == TRAN_DRAGON)
        && species == SP_YELLOW_DRACONIAN)
    {
        return true;
    }

    if (form_keeps_mutations()
        && player_mutation_level(MUT_YELLOW_SCALES) >= 3)
    {
        return true;
    }

    return actor::res_corr(calc_unid, items);
}

int player_res_acid(bool calc_unid, bool items)
{
    return you.res_corr(calc_unid, items) ? 1 : 0;
}

int player_res_electricity(bool calc_unid, bool temp, bool items)
{
    int re = 0;

    if (items)
    {
        // staff
        re += you.wearing(EQ_STAFF, STAFF_AIR, calc_unid);

        // body armour:
        const item_def *body_armour = you.slot_item(EQ_BODY_ARMOUR);
        if (body_armour)
            re += armour_type_prop(body_armour->sub_type, ARMF_RES_ELEC);

        // randart weapons:
        re += you.scan_artefacts(ARTP_ELECTRICITY, calc_unid);

        // dragonskin cloak: 0.5 to draconic resistances
        if (calc_unid && player_equip_unrand(UNRAND_DRAGONSKIN) && coinflip())
            re++;
    }

    // mutations:
    re += player_mutation_level(MUT_THIN_METALLIC_SCALES, temp) == 3 ? 1 : 0;
    re += player_mutation_level(MUT_SHOCK_RESISTANCE, temp);
    re -= player_mutation_level(MUT_SHOCK_VULNERABILITY, temp);

    if (temp)
    {
        if (you.attribute[ATTR_DIVINE_LIGHTNING_PROTECTION])
            return 3;

        if (you.duration[DUR_RESISTANCE])
            re++;

        if (you.duration[DUR_QAZLAL_ELEC_RES])
            re++;

        // transformations:
        if (get_form()->res_elec())
            re++;
    }

    if (re > 1)
        re = 1;

    return re;
}

/**
 * Is the player character immune to torment?
 *
 * @param random    Whether to include unreliable effects (stochastic resist)
 * @return          Whether the player resists a given instance of torment; if
 *                  random is passed, the result may vary from call to call.
 */
bool player_res_torment(bool random)
{
    if (player_mutation_level(MUT_TORMENT_RESISTANCE))
        return true;

    if (random
        && x_chance_in_y(player_mutation_level(MUT_STOCHASTIC_TORMENT_RESISTANCE), 4)
        )
    {
        return true;
    }

    return get_form()->res_neg() == 3
           || you.species == SP_VAMPIRE && you.hunger_state <= HS_STARVING
           || you.petrified()
           #if TAG_MAJOR_VERSION == 34
           || player_equip_unrand(UNRAND_ETERNAL_TORMENT)
#endif
        ;
}

// Kiku protects you from torment to a degree.
bool player_kiku_res_torment()
{
    // no protection during pain branding weapon
    return have_passive(passive_t::resist_torment)
           && !(you_worship(GOD_KIKUBAAQUDGHA) && you.gift_timeout);
}

// If temp is set to false, temporary sources or resistance won't be counted.
int player_res_poison(bool calc_unid, bool temp, bool items)
{
    switch (you.undead_state(temp))
    {
        case US_ALIVE:
            break;
        case US_HUNGRY_DEAD: //ghouls
        case US_UNDEAD: // mummies & lichform
            return 3;
        case US_SEMI_UNDEAD: // vampire
            if (you.hunger_state <= HS_STARVING) // XXX: && temp?
                return 3;
            break;
    }

    if (you.is_artificial(temp)
        || temp && get_form()->res_pois() == 3
        || items && player_equip_unrand(UNRAND_OLGREB)
        || temp && you.duration[DUR_DIVINE_STAMINA])
    {
        return 3;
    }

    int rp = 0;

    if (items)
    {
        // rings of poison resistance
        rp += you.wearing(EQ_RINGS, RING_POISON_RESISTANCE, calc_unid);

        // Staves
        rp += you.wearing(EQ_STAFF, STAFF_POISON, calc_unid);

        // ego armour:
        rp += you.wearing_ego(EQ_ALL_ARMOUR, SPARM_POISON_RESISTANCE);

        // body armour:
        const item_def *body_armour = you.slot_item(EQ_BODY_ARMOUR);
        if (body_armour)
            rp += armour_type_prop(body_armour->sub_type, ARMF_RES_POISON);

        // rPois+ artefacts
        rp += you.scan_artefacts(ARTP_POISON, calc_unid);

        // dragonskin cloak: 0.5 to draconic resistances
        if (calc_unid && player_equip_unrand(UNRAND_DRAGONSKIN) && coinflip())
            rp++;
    }

    // mutations:
    rp += player_mutation_level(MUT_POISON_RESISTANCE, temp);
    rp -= player_mutation_level(MUT_POISON_VULNERABILITY, temp);

    rp += player_mutation_level(MUT_SLIMY_GREEN_SCALES, temp) == 3 ? 1 : 0;

    // Only thirsty vampires are naturally poison resistant.
    // XXX: && temp?
    if (you.species == SP_VAMPIRE && you.hunger_state < HS_SATIATED)
        rp++;

    if (temp)
    {
        // potions/cards:
        if (you.duration[DUR_RESISTANCE])
            rp++;

        if (get_form()->res_pois() > 0)
            rp++;
    }

    // Cap rPois at + before vulnerability effects are applied
    // (so carrying multiple rPois effects is never useful)
    rp = min(1, rp);

    if (temp)
    {
        if (get_form()->res_pois() < 0)
            rp--;

        if (you.duration[DUR_POISON_VULN])
            rp--;
    }

    // don't allow rPois--, etc.
    rp = max(-1, rp);

    return rp;
}

int player_res_sticky_flame(bool calc_unid, bool temp, bool items)
{
    int rsf = 0;

    if (you.species == SP_MOTTLED_DRACONIAN)
        rsf++;

    const item_def *body_armour = you.slot_item(EQ_BODY_ARMOUR);
    if (body_armour)
        rsf += armour_type_prop(body_armour->sub_type, ARMF_RES_STICKY_FLAME);

    // dragonskin cloak: 0.5 to draconic resistances
    if (items && calc_unid
        && player_equip_unrand(UNRAND_DRAGONSKIN) && coinflip())
    {
        rsf++;
    }

    if (get_form()->res_sticky_flame())
        rsf++;

    if (rsf > 1)
        rsf = 1;

    return rsf;
}

int player_spec_death()
{
    int sd = 0;

    // Staves
    sd += you.wearing(EQ_STAFF, STAFF_DEATH);

    // species:
    sd += player_mutation_level(MUT_NECRO_ENHANCER);

    // transformations:
    if (you.form == TRAN_LICH)
        sd++;

    return sd;
}

int player_spec_fire()
{
    int sf = 0;

    // staves:
    sf += you.wearing(EQ_STAFF, STAFF_FIRE);

    // rings of fire:
    sf += you.wearing(EQ_RINGS, RING_FIRE);

    if (you.species == SP_LAVA_ORC && temperature_effect(LORC_LAVA_BOOST))
        sf++;

    if (you.duration[DUR_FIRE_SHIELD])
        sf++;

    return sf;
}

int player_spec_cold()
{
    int sc = 0;

    // staves:
    sc += you.wearing(EQ_STAFF, STAFF_COLD);

    // rings of ice:
    sc += you.wearing(EQ_RINGS, RING_ICE);

    if (you.species == SP_LAVA_ORC
        && (temperature_effect(LORC_LAVA_BOOST)
            || temperature_effect(LORC_FIRE_BOOST)))
    {
        sc--;
    }

    return sc;
}

int player_spec_earth()
{
    int se = 0;

    // Staves
    se += you.wearing(EQ_STAFF, STAFF_EARTH);
    
    if (you.species == SP_LAVA_ORC && temperature_effect(LORC_LAVA_BOOST)) 
    	se++;

    return se;
}

int player_spec_light()
{
    int sa = 0;

    // Staves
    sa += you.wearing(EQ_STAFF, STAFF_LIGHT);

    return sa;
}

int player_spec_darkness()
{
    int sa = 0;

    // Staves
    sa += you.wearing(EQ_STAFF, STAFF_DARKNESS);

    return sa;
}

int player_spec_time()
{
    int sa = 0;

    // Staves
    sa += you.wearing(EQ_STAFF, STAFF_TIME);

    return sa;
}

int player_spec_air()
{
    int sa = 0;

    // Staves
    sa += you.wearing(EQ_STAFF, STAFF_AIR);

    return sa;
}

int player_spec_conj()
{
    int sc = 0;

    // Staves
    sc += you.wearing(EQ_STAFF, STAFF_CONJURATION);

    return sc;
}

int player_spec_hex()
{
    return 0;
}

int player_spec_charm()
{
    // Nothing, for the moment.
    return 0;
}

int player_spec_summ()
{
    int ss = 0;

    // Staves
    ss += you.wearing(EQ_STAFF, STAFF_SUMMONING);

    return ss;
}

int player_spec_poison()
{
    int sp = 0;

    // Staves
    sp += you.wearing(EQ_STAFF, STAFF_POISON);

    if (player_equip_unrand(UNRAND_OLGREB))
        sp++;

    return sp;
}

int player_energy()
{
    int pe = 0;

    // Staves
    pe += you.wearing(EQ_STAFF, STAFF_ENERGY);

    return pe;
}

// If temp is set to false, temporary sources of resistance won't be
// counted.
int player_prot_life(bool calc_unid, bool temp, bool items)
{
    int pl = 0;

    // Hunger is temporary, true, but that's something you can control,
    // especially as life protection only increases the hungrier you
    // get.
    if (you.species == SP_VAMPIRE)
    {
        switch (you.hunger_state)
        {
        case HS_FAINTING:
        case HS_STARVING:
        case HS_NEAR_STARVING:
            pl = 3;
            break;
        case HS_VERY_HUNGRY:
        case HS_HUNGRY:
            pl = 2;
            break;
        case HS_SATIATED:
            pl = 1;
            break;
        default:
            break;
        }
    }

    // Same here. Your piety status, and, hence, TSO's protection, is
    // something you can more or less control.
    if (you_worship(GOD_SHINING_ONE))
    {
        if (you.piety >= piety_breakpoint(1))
            pl++;
        if (you.piety >= piety_breakpoint(3))
            pl++;
        if (you.piety >= piety_breakpoint(5))
            pl++;
    }

    if (temp)
    {
        pl += get_form()->res_neg();

        // completely stoned, unlike statue which has some life force
        if (you.petrified())
            pl += 3;
    }

    if (items)
    {
        // rings
        pl += you.wearing(EQ_RINGS, RING_LIFE_PROTECTION, calc_unid);

        // armour (checks body armour only)
        pl += you.wearing_ego(EQ_ALL_ARMOUR, SPARM_POSITIVE_ENERGY);

        // pearl dragon counts
        const item_def *body_armour = you.slot_item(EQ_BODY_ARMOUR);
        if (body_armour)
            pl += armour_type_prop(body_armour->sub_type, ARMF_RES_NEG);

        // randart wpns
        pl += you.scan_artefacts(ARTP_NEGATIVE_ENERGY, calc_unid);

        // dragonskin cloak: 0.5 to draconic resistances
        if (calc_unid && player_equip_unrand(UNRAND_DRAGONSKIN) && coinflip())
            pl++;

        pl += you.wearing(EQ_STAFF, STAFF_DEATH, calc_unid);
    }

    // undead/demonic power
    pl += player_mutation_level(MUT_NEGATIVE_ENERGY_RESISTANCE, temp);

    pl = min(3, pl);

    return pl;
}

// New player movement speed system... allows for a bit more than
// "player runs fast" and "player walks slow" in that the speed is
// actually calculated (allowing for centaurs to get a bonus from
// swiftness and other such things). Levels of the mutation now
// also have meaning (before they all just meant fast). Most of
// this isn't as fast as it used to be (6 for having anything), but
// even a slight speed advantage is very good... and we certainly don't
// want to go past 6 (see below). -- bwr
int player_movement_speed()
{
    int mv = 1100;

    if (in_quick_mode() && you.religion != GOD_CHEIBRIADOS)
    {
        mv = 900;
    }

    if (player_is_very_tired(true))
        mv += 100;

    if (you.rune_curse_active[RUNE_TOMB])
        mv += 100;

    // transformations
    if (in_quick_mode())
    {
        if (you.form == TRAN_BAT)
            mv = 500;
        else if (you.form == TRAN_PIG || you.form == TRAN_SPIDER)
            mv = 700;
        else if (you.form == TRAN_PORCUPINE || you.form == TRAN_WISP)
            mv = 800;
        else if (you.fishtail || you.form == TRAN_HYDRA && you.in_water())
            mv = 600;

        int run_bonus = you.run() ? 1 : 0;
        run_bonus += you.scan_artefacts(ARTP_RUNNING);
        mv -= run_bonus * 200;

        // Tengu can move slightly faster when flying.
        if (you.tengu_flight())
            mv -= 200 + effective_xl() * 14;
    }

    if (you.liquefied_ground() && you.species != SP_LAVA_ORC)
        mv += 300;

	if (you.species == SP_LAVA_ORC) {
		if (you.temperature < TEMP_COOL) {
			mv += 100;
		}

        if (you.temperature >= TEMP_ROOM) {
            mv -= 100;
        }

        if (in_quick_mode())
        {
            if (you.temperature >= TEMP_HOT) {
                mv -= 100;
            }
        }
	}

    mv += you.wearing_ego(EQ_ALL_ARMOUR, SPARM_PONDEROUSNESS) * 100;

    // Cheibriados
    if (have_passive(passive_t::slowed))
        mv += 200 + min(you.piety * 5, 800);
    else if (player_under_penance(GOD_CHEIBRIADOS))
        mv += 200 + min(you.piety_max[GOD_CHEIBRIADOS] * 5, 800);

    if (you.duration[DUR_FROZEN])
        mv += 400;

    if (you.duration[DUR_GRASPING_ROOTS])
        mv += 300;

    if (you.duration[DUR_ICY_ARMOUR])
        mv += 100; // as ponderous

    if (in_quick_mode())
        mv -= player_mutation_level(MUT_FAST) * 150;

    mv += player_mutation_level(MUT_SLOW) * 150;

    if (you.duration[DUR_SWIFTNESS] > 0 && !you.in_liquid())
    {
        if (you.attribute[ATTR_SWIFTNESS] > 0)
          mv = mv * 3 / 4;
        else
          mv = mv * 4 / 3;
    }

    mv = div_rand_round(mv, 100);

    // We'll use the old value of six as a minimum, with haste this could
    // end up as a speed of three, which is about as fast as we want
    // the player to be able to go (since 3 is 3.33x as fast and 2 is 5x,
    // which is a bit of a jump, and a bit too fast) -- bwr
    // Currently Haste takes 6 to 4, which is 2.5x as fast as delay 10
    // and still seems plenty fast. -- elliptic

    if (mv < FASTEST_PLAYER_MOVE_SPEED)
        mv = FASTEST_PLAYER_MOVE_SPEED;

    return mv;
}

const int player_adjust_evoc_power(const int power)
{
    return stepdown_spellpower(100*apply_enhancement(power, you.spec_evoke()));
}

const int player_adjust_invoc_power(const int power)
{
    return apply_enhancement(power, you.spec_invoc());
}

// This function differs from the above in that it's used to set the
// initial time_taken value for the turn. Everything else (movement,
// spellcasting, combat) applies a ratio to this value.
int player_speed()
{
    int ps = 10;

    // When paralysed, speed is irrelevant.
    if (you.cannot_act())
        return ps;

    if (you.duration[DUR_SLOW] || have_stat_zero())
        ps = haste_mul(ps);

    if (you.duration[DUR_BERSERK] && !have_passive(passive_t::no_haste))
        ps = berserk_div(ps);
    else if (you.duration[DUR_HASTE])
        ps = haste_div(ps);

    if (you.form == TRAN_STATUE || you.duration[DUR_PETRIFYING])
    {
        ps *= 15;
        ps /= 10;
    }

    return ps;
}

// Get level of player mutation, ignoring mutations with an activity level
// less than minact.
static int _mut_level(mutation_type mut, mutation_activity_type minact)
{
    const int mlevel = you.mutation[mut];

    const mutation_activity_type active = mutation_activity_level(mut);

    if (active >= minact)
        return mlevel;

    return 0;
}

// Output level of player mutation. If temp is true (the default), take into
// account the suppression of mutations by changes of form.
int player_mutation_level(mutation_type mut, bool temp)
{
    return _mut_level(mut, temp ? MUTACT_PARTIAL : MUTACT_INACTIVE);
}

bool player_ephemeral_passthrough(const string &whatIsAttacking, bool showMessage)
{
    const int ephem = player_mutation_level(MUT_EPHEMERAL);
    if (ephem > 0 && one_chance_in(12)
    || ephem > 1 && one_chance_in(6)
    || ephem > 2 && one_chance_in(3))
    {
    	if(showMessage)
    		mprf("%s passes right through your ephemeral form!", whatIsAttacking.c_str());
        return true;
    }
    return false;
}

static int _player_armour_beogh_bonus(const item_def& item)
{
    if (item.base_type != OBJ_ARMOUR)
        return 0;

    int bonus = 0;

    if (have_passive(passive_t::bonus_ac))
    {
        if (you.piety >= piety_breakpoint(5))
            bonus = 10;
        else if (you.piety >= piety_breakpoint(4))
            bonus = 8;
        else if (you.piety >= piety_breakpoint(2))
            bonus = 6;
        else if (you.piety >= piety_breakpoint(0))
            bonus = 4;
        else
            bonus = 2;
    }

    return bonus;
}

bool is_effectively_light_armour(const item_def *item)
{
    return !item
           || (abs(property(*item, PARM_EVASION)) / 10 < 5);
}

bool player_effectively_in_light_armour()
{
    const item_def *armour = you.slot_item(EQ_BODY_ARMOUR, false);
    return is_effectively_light_armour(armour);
}

// This function returns true if the player has a radically different
// shape... minor changes like blade hands don't count, also note
// that lich transformation doesn't change the character's shape
// (so we end up with Naga-liches, Spriggan-liches, Minotaur-liches)
// it just makes the character undead (with the benefits that implies). - bwr
bool player_is_shapechanged()
{
    if (you.form == TRAN_NONE
        || you.form == TRAN_BLADE_HANDS
        || you.form == TRAN_LICH
        || you.form == TRAN_SHADOW
        || you.form == TRAN_APPENDAGE)
    {
        return false;
    }

    return true;
}

// An evasion factor based on the player's body size, smaller == higher
// evasion size factor.
static int _player_evasion_size_factor(bool base = false)
{
    // XXX: you.body_size() implementations are incomplete, fix.
    const size_type size = you.body_size(PSIZE_BODY, base);
    return 2 * (SIZE_MEDIUM - size);
}

// Determines racial shield penalties (formicids get a bonus compared to
// other medium-sized races)
int player_shield_racial_factor()
{
    return max(1, 5 + (you.species == SP_FORMICID ? -2 // Same as trolls/centaurs/etc.
                                                  : _player_evasion_size_factor(true)));
}

// The total EV penalty to the player for all their worn armour items
// with a base EV penalty (i.e. EV penalty as a base armour property,
// not as a randart property).
static int _player_adjusted_evasion_penalty(const int scale)
{
    int piece_armour_evasion_penalty = 0;

    // Some lesser armours have small penalties now (barding).
    for (int i = EQ_MIN_ARMOUR; i < EQ_MAX_ARMOUR; i++)
    {
        if (i == EQ_SHIELD || !you.slot_item(static_cast<equipment_type>(i)))
            continue;

        // [ds] Evasion modifiers for armour are negatives, change
        // those to positive for penalty calc.
        const int penalty = (-property(you.inv1[you.equip[i]], PARM_EVASION))/3;
        if (penalty > 0)
            piece_armour_evasion_penalty += penalty;
    }

    return piece_armour_evasion_penalty * scale / 10 +
           you.adjusted_body_armour_penalty(scale);
}

// EV bonuses that work even when helpless.
static int _player_para_evasion_bonuses(ev_ignore_type evit)
{
    int evbonus = 0;

    if (player_mutation_level(MUT_DISTORTION_FIELD) > 0)
        evbonus += player_mutation_level(MUT_DISTORTION_FIELD) * 2;

    return evbonus;
}

// Player EV bonuses for various effects and transformations. This
// does not include tengu/merfolk EV bonuses for flight/swimming.
static int _player_evasion_bonuses(ev_ignore_type evit)
{
    int evbonus = 0;

    if (you.duration[DUR_AGILITY])
        evbonus += AGILITY_BONUS;

    evbonus += you.wearing(EQ_RINGS_PLUS, RING_EVASION);

    if (you.wearing_ego(EQ_WEAPON, SPWPN_EVASION))
        evbonus += 10;

    evbonus += you.scan_artefacts(ARTP_EVASION);

    // mutations
    if (_mut_level(MUT_ICY_BLUE_SCALES, MUTACT_FULL) > 1)
        evbonus--;
    if (_mut_level(MUT_MOLTEN_SCALES, MUTACT_FULL) > 1)
        evbonus--;
    evbonus += max(0, player_mutation_level(MUT_GELATINOUS_BODY) - 1);

    // transformation penalties/bonuses not covered by size alone:
    if (player_mutation_level(MUT_SLOW_REFLEXES))
        evbonus -= player_mutation_level(MUT_SLOW_REFLEXES) * 3;

    return evbonus;
}

// Player EV scaling for being flying tengu or swimming merfolk.
static int _player_scale_evasion(int prescaled_ev, const int scale)
{
    if (you.duration[DUR_PETRIFYING] || you.caught())
        prescaled_ev /= 2;
    else if (you.duration[DUR_GRASPING_ROOTS])
        prescaled_ev = prescaled_ev * 2 / 3;

    // Merfolk get a 25% evasion bonus in water.
    if (you.fishtail)
    {
        const int ev_bonus = max(2 * scale, prescaled_ev / 4);
        return prescaled_ev + ev_bonus;
    }

    // Flying Tengu get a 20% evasion bonus.
    if (you.tengu_flight())
    {
        const int ev_bonus = max(1 * scale, prescaled_ev * effective_xl() / 60);
        return prescaled_ev + ev_bonus;
    }

    return prescaled_ev;
}

/**
 * What is the player's bonus to EV from dodging when not paralyzed, after
 * accounting for size & body armour penalties?
 *
 * @param scale     A scale to multiply the result by, to avoid precision loss.
 * @return          A bonus to EV, multiplied by the scale.
 */
static int _player_armour_adjusted_dodge_bonus(int scale)
{
    int ev = 0;
    const int size_factor = _player_evasion_size_factor();

    const int ev_dex = you.dex() - 10;
    ev += ev_dex * scale / 2;

    const int dodge_bonus = you.skill(SK_DODGING, scale);
    ev += dodge_bonus / 2;

    ev = ev * 10 / (10 + size_factor);

    const int armour_dodge_penalty = you.unadjusted_body_armour_penalty() * 10 / (you.strength() + 1);
    ev -= armour_dodge_penalty * scale;
    ev = max(0, ev);

    return ev;
}

// Total EV for player using the revised 0.6 evasion model.
static int _player_evasion(ev_ignore_type evit)
{
    const int scale = 100;

    const int size_factor = _player_evasion_size_factor();
    // Repulsion fields and size are all that matters when paralysed or
    // at 0 dex.
    const bool can_not_move = (you.cannot_move() || you.duration[DUR_CLUMSY] || you.form == TRAN_TREE)
                              && !(evit & EV_IGNORE_HELPLESS);

    int ev = 0;
    
    const int repulsion_ev = _player_para_evasion_bonuses(evit) * scale;
    ev += repulsion_ev;

    if (can_not_move)
    {
        const int paralysed_base_ev = 2 + size_factor / 2;
        ev += paralysed_base_ev * scale;
    }
    else
    {
        const int size_base_ev = 10 + size_factor;
        ev += size_base_ev * scale;
    }

    const int vertigo_penalty = you.duration[DUR_VERTIGO] ? 10 : 0;
    ev -= vertigo_penalty * scale;

    const int dodge_bonus = _player_armour_adjusted_dodge_bonus(scale);
    ev += dodge_bonus;

    const int evasion_penalty = _player_adjusted_evasion_penalty(scale);
    ev -= evasion_penalty;

    const int shield_penalty = you.adjusted_shield_penalty(scale);
    ev -= shield_penalty;

    const int evasion_bonuses = _player_evasion_bonuses(evit);
    ev += evasion_bonuses * scale;

    ev = unscale_round_up(ev, scale);
    ev = max(1, ev);

    return ev;
}

// Returns the spellcasting penalty (increase in spell failure) for the
// player's worn body armour and shield.
int player_armour_shield_spell_penalty()
{
    const int scale = 100;

    const int body_armour_penalty =
        max(19 * you.adjusted_body_armour_penalty(scale), 0);

    const int total_penalty = body_armour_penalty
                 + 19 * you.adjusted_shield_penalty(scale);

    return max(total_penalty, 0) / scale;
}

/**
 * How many spell-success-chance-boosting ('wizardry') effects can the player
 * apply to the given spell?
 *
 * @param spell     The type of spell being cast.
 * @return          The number of relevant wizardry effects.
 */
int player_wizardry(spell_type spell)
{
    return you.wearing(EQ_RINGS, RING_WIZARDRY)
           + you.wearing(EQ_STAFF, STAFF_WIZARDRY);
}

/**
 * Calculate the SH value used internally.
 *
 * Exactly twice the value displayed to players, for legacy reasons.
 * @return      The player's current SH value.
 */
int player_shield_class()
{
    int shield = 0;

    if (you.incapacitated())
        return 0;

    if (you.shield())
    {
        const item_def& item = you.inv1[you.equip[EQ_SHIELD]];
        int size_factor = (you.body_size(PSIZE_TORSO) - SIZE_MEDIUM)
                        * (item.sub_type - ARM_LARGE_SHIELD);
        int base_shield = property(item, PARM_AC) * 2 + size_factor;

        int beogh_bonus = _player_armour_beogh_bonus(item);

        // bonus applied only to base, see above for effect:
        shield += base_shield * 50;
        shield += base_shield * you.skill(SK_SHIELDS, 5) / 2;
        shield += base_shield * beogh_bonus * 10 / 6;

        shield += item.plus * 200;

        shield += you.skill(SK_SHIELDS, 38)
                + min(you.skill(SK_SHIELDS, 38), 3 * 38);

        int stat = 0;
        if (item.sub_type == ARM_BUCKLER)
            stat = you.dex() * 38;
        else if (item.sub_type == ARM_LARGE_SHIELD)
            stat = you.dex() * 12 + you.strength() * 26;
        else
            stat = you.dex() * 19 + you.strength() * 19;
        stat = stat * (base_shield + 13) / 26;

        shield += stat;
    }
    else if (you.duration[DUR_MAGIC_SHIELD])
        shield += 900 + player_adjust_evoc_power(you.skill(SK_EVOCATIONS, 75));

    // mutations
    // +2, +3, +4 (displayed values)
    shield += (player_mutation_level(MUT_LARGE_BONE_PLATES) > 0
               ? player_mutation_level(MUT_LARGE_BONE_PLATES) * 200 + 200
               : 0);

    shield += qazlal_sh_boost() * 100;
    shield += tso_sh_boost() * 100;
    shield += _bone_armour_bonus() * 2;
    shield += you.wearing(EQ_AMULET_PLUS, AMU_REFLECTION) * 200;
    shield += you.scan_artefacts(ARTP_SHIELDING) * 200;
    shield = (shield + 50) / 100;

    shield = player_sh_modifier(shield);

    return shield;
}

/**
 * Calculate the SH value that should be displayed to players.
 *
 * Exactly half the internal value, for legacy reasons.
 * @return      The SH value to be displayed.
 */
int player_displayed_shield_class()
{
    return player_shield_class() / 2;
}

/**
 * Does the player have 'omnireflection' (the ability to reflect piercing
 * effects and enchantments)?
 *
 * @return      Whether the player has the Warlock's Mirror equipped.
 */
bool player_omnireflects()
{
    return player_equip_unrand(UNRAND_WARLOCK_MIRROR);
}

/**
 * Does the player take halved ability damage?
 *
 * @param calc_unid     Whether to include properties of worn but unidentified
 *                      items in the calculation. (Probably irrelevant.)
 * @return              Whether the player has SustAt.
 */
bool player_sust_attr(bool calc_unid)
{
    return you.wearing(EQ_RINGS, RING_SUSTAIN_ATTRIBUTES, calc_unid)
           || you.scan_artefacts(ARTP_SUSTAT)
           || player_mutation_level(MUT_SUSTAIN_ATTRIBUTES);
}

void forget_map(bool rot)
{
    ASSERT(!crawl_state.game_is_arena());

    // If forgetting was intentional, clear the travel trail.
    if (!rot)
        clear_travel_trail();

    // Labyrinth and the Abyss use special rotting rules.
    const bool rot_resist = player_in_branch(BRANCH_LABYRINTH)
                                && you.species == SP_MINOTAUR
                            || player_in_branch(BRANCH_ABYSS)
                                && have_passive(passive_t::map_rot_res_abyss);
    const double geometric_chance = 0.99;
    const int radius = (rot_resist ? 200 : 100);

    const int scalar = 0xFF;
    for (rectangle_iterator ri(0); ri; ++ri)
    {
        const coord_def &p = *ri;
        if (!env.map_knowledge(p).known() || you.see_cell(p))
            continue;

        if (rot)
        {
            const int dist = grid_distance(you.pos(), p);
            int chance = pow(geometric_chance,
                             max(1, (dist * dist - radius) / 40)) * scalar;
            if (x_chance_in_y(chance, scalar))
                continue;
        }

        if (you.see_cell(p))
            continue;

        env.map_knowledge(p).clear();
        if (env.map_forgotten.get())
            (*env.map_forgotten.get())(p).clear();
        StashTrack.update_stash(p);
#ifdef USE_TILE
        tile_forget_map(p);
#endif
    }

    ash_detect_portals(is_map_persistent());
#ifdef USE_TILE
    tiles.update_minimap_bounds();
#endif
}

static void _remove_temp_mutation()
{
    int num_remove = min(you.attribute[ATTR_TEMP_MUTATIONS],
        max(you.attribute[ATTR_TEMP_MUTATIONS] * 5 / 12 - random2(3),
        1 + random2(3)));

    if (num_remove >= you.attribute[ATTR_TEMP_MUTATIONS])
        mprf(MSGCH_DURATION, "You feel the corruption within you wane completely.");
    else
        mprf(MSGCH_DURATION, "You feel the corruption within you wane somewhat.");

    for (int i = 0; i < num_remove; ++i)
        delete_temp_mutation();

    if (you.attribute[ATTR_TEMP_MUTATIONS] > 0)
        you.attribute[ATTR_TEMP_MUT_XP] += temp_mutation_roll();
}

static void _recover_stat()
{
    FixedVector<int, NUM_STATS> recovered_stats(0);

    while (you.attribute[ATTR_STAT_LOSS_XP] <= 0)
    {
        stat_type stat = random_lost_stat();
        ASSERT(stat != NUM_STATS);

        recovered_stats[stat]++;

        // Very heavily drained stats recover faster.
        if (you.stat(stat, false) < 0)
            recovered_stats[stat] += random2(-you.stat(stat, false) / 2);

        bool still_drained = false;
        for (int i = 0; i < NUM_STATS; ++i)
            if (you.stat_loss[i] - recovered_stats[i] > 0)
                still_drained = true;

        if (still_drained)
            you.attribute[ATTR_STAT_LOSS_XP] += stat_loss_roll();
        else
            break;
    }

    for (int i = 0; i < NUM_STATS; ++i)
        if (recovered_stats[i] > 0)
            restore_stat((stat_type) i, recovered_stats[i], false, true);
}

int get_exp_progress()
{
    if (you.experience_level >= you.get_max_xl())
        return 0;

    const int current = exp_needed(you.experience_level);
    const int next    = exp_needed(you.experience_level + 1);
    if (next == current)
        return 0;
    return (you.experience - current) * 100 / (next - current);
}

static void _recharge_xp_evokers(int exp)
{
    FixedVector<item_def*, NUM_MISCELLANY> evokers(nullptr);
    list_charging_evokers(evokers);

    int xp_factor = max(min((int)exp_needed(you.experience_level+1, 0) * 2 / 7,
                             you.experience_level * 425),
                        you.experience_level*4 + 30)
                    / (3 + you.skill_rdiv(SK_EVOCATIONS, 2, 13));

    for (int i = 0; i < NUM_MISCELLANY; ++i)
    {
        item_def* evoker = evokers[i];
        if (!evoker)
            continue;

        int &debt = evoker_debt(evoker->sub_type);
        if (debt == 0)
            continue;

        debt = max(0, debt - div_rand_round(exp, xp_factor));
        if (debt == 0)
            mprf("%s has recharged.", evoker->name(DESC_YOUR).c_str());
    }
}

/// Make progress toward the abyss spawning an exit/stairs.
static void _reduce_abyss_xp_timer(int exp)
{
    if (!player_in_branch(BRANCH_ABYSS))
        return;

    const int xp_factor =
        max(min((int)exp_needed(you.experience_level+1, 0) / 7,
                you.experience_level * 425),
            you.experience_level*2 + 15) / 5;

    if (!you.props.exists(ABYSS_STAIR_XP_KEY))
        you.props[ABYSS_STAIR_XP_KEY] = EXIT_XP_COST;
    const int reqd_xp = you.props[ABYSS_STAIR_XP_KEY].get_int();
    const int new_req = reqd_xp - div_rand_round(exp, xp_factor);
    dprf("reducing xp timer from %d to %d (factor = %d)",
         reqd_xp, new_req, xp_factor);
    you.props[ABYSS_STAIR_XP_KEY].get_int() = new_req;
}

const int _experience_for_this_floor(int multiplier) {
    int exp = 0;

    if (!is_safe_branch(you.where_are_you)
        && !(you.where_are_you == BRANCH_DUNGEON && you.depth == 1)
            )
    {
        int how_deep = absdungeon_depth(you.where_are_you, you.depth);

        if (Options.exp_based_on_player_level)
            exp = exp_needed(you.experience_level + 1, 0) - exp_needed(you.experience_level, 0);
        else
            exp = exp_needed(min(1, how_deep * 2 / 3), 0);

        exp *= multiplier;
        exp = div_rand_round(exp, 100);
        exp = max(5 * how_deep, exp);
    }

    return exp;
}

const int potion_experience_for_this_floor()
{
    int exp = _experience_for_this_floor(Options.exp_percent_from_potions);

    exp = max(exp, you.max_exp);
    you.max_exp = max(you.max_exp, exp);

    return exp;
}

const int floor_experience_for_this_floor()
{
    int exp = _experience_for_this_floor(Options.exp_percent_from_new_branch_floor);

    return exp;
}

void gain_potion_exp(bool skip_training)
{
    const int exp = potion_experience_for_this_floor();
    gain_exp(abs(exp), nullptr, false, exp < 0);
}

void gain_floor_exp()
{
    const int exp = floor_experience_for_this_floor();
    gain_exp(abs(exp), nullptr, false, exp < 0);
}

void gain_exp(unsigned int exp_gained, unsigned int* actual_gain, bool from_monster, bool exp_loss, bool skip_training)
{
    if (actual_gain != nullptr)
        *actual_gain = 0;

    if (from_monster)
    {
        exp_gained = div_rand_round(exp_gained * abs(Options.exp_percent_from_monsters), 100);
        exp_loss = Options.exp_percent_from_monsters < 0;
    }

    if (player_under_penance(GOD_HEPLIAKLQANA))
        return; // no xp for you!

    const unsigned int old_exp = you.experience;

    if (!exp_loss)
    {
        vector<god_type> xp_gods;
        for (god_iterator it; it; ++it)
        {
            if (xp_penance(*it))
                xp_gods.push_back(*it);
        }

        if (!xp_gods.empty())
        {
            god_type god = xp_gods[random2(xp_gods.size())];
            reduce_xp_penance(god, exp_gained);
        }

        dprf("gain_exp: %d", exp_gained);

        if (you.transfer_skill_points > 0)
        {
            // Can happen if the game got interrupted during target skill choice.
            if (is_invalid_skill(you.transfer_to_skill))
            {
                you.transfer_from_skill = SK_NONE;
                you.transfer_skill_points = 0;
                you.transfer_total_skill_points = 0;
            }
            else
            {
                int amount = exp_gained * 20
                             / calc_skill_cost(you.skill_cost_level);
                if (amount >= 20 || one_chance_in(20 - amount))
                {
                    amount = max(20, amount);
                    transfer_skill_points(you.transfer_from_skill,
                                          you.transfer_to_skill, amount, false);
                }
            }
        }
    }

    const bool can_gain_experience_here =
        !(is_double_deep_branch(you.where_are_you) && in_lower_half_of_branch())
        && Options.exp_percent_from_monsters
        && !player_in_branch(BRANCH_ABYSS)
        || !from_monster;

    if (!can_gain_experience_here)
        skip_training = true;

    if (can_gain_experience_here)
    {
        int adjusted_gain = exp_gained;
        if (crawl_state.difficulty == DIFFICULTY_EASY)
            adjusted_gain = div_rand_round(adjusted_gain * 5, 3);

        if (crawl_state.difficulty == DIFFICULTY_STANDARD)
            adjusted_gain = div_rand_round(adjusted_gain * 4, 3);

        if (crawl_state.difficulty == DIFFICULTY_NIGHTMARE)
            adjusted_gain = div_rand_round(adjusted_gain * 3, 4);

        if (exp_loss)
        {
            if (you.experience < adjusted_gain)
                you.experience = 0;
            else
                you.experience -= adjusted_gain;
        }
        else
            if (you.experience + adjusted_gain > (unsigned int)MAX_EXP_TOTAL)
                you.experience = MAX_EXP_TOTAL;
            else
                you.experience += adjusted_gain;
    }

    if (!exp_loss)
    {
        you.attribute[ATTR_EVOL_XP] += exp_gained;
        for (god_iterator it; it; ++it)
        {
            if (active_penance(*it))
            {
                you.attribute[ATTR_GOD_WRATH_XP] -= exp_gained;
                while (you.attribute[ATTR_GOD_WRATH_XP] < 0)
                {
                    you.attribute[ATTR_GOD_WRATH_COUNT]++;
                    set_penance_xp_timeout();
                }
                break;
            }
        }

        if (crawl_state.game_is_sprint())
            exp_gained = sprint_modify_exp(exp_gained);

        if (!skip_training && (Options.exp_percent_from_monsters || !from_monster))
        {
            you.exp_available += exp_gained;

            train_skills();
            while (check_selected_skills()
                   && you.exp_available >= calc_skill_cost(you.skill_cost_level))
            {
                train_skills();
            }

            if (you.exp_available >= calc_skill_cost(you.skill_cost_level))
                you.exp_available = calc_skill_cost(you.skill_cost_level);
        }
    }

    level_change();

    if (actual_gain != nullptr)
        *actual_gain = you.experience - old_exp;

    if (!exp_loss)
    {
        if (you.attribute[ATTR_TEMP_MUTATIONS] > 0)
        {
            you.attribute[ATTR_TEMP_MUT_XP] -= exp_gained;
            if (you.attribute[ATTR_TEMP_MUT_XP] <= 0)
                _remove_temp_mutation();
        }

        if (you.attribute[ATTR_STAT_LOSS_XP] > 0)
        {
            int loss = div_rand_round(exp_gained * 3 / 2,
                                      max(1, calc_skill_cost(you.skill_cost_level) - 3));
            you.attribute[ATTR_STAT_LOSS_XP] -= loss * (2 + player_sust_attr() ? 2 : 0);

            dprf("Stat loss points: %d", you.attribute[ATTR_STAT_LOSS_XP]);
            if (you.attribute[ATTR_STAT_LOSS_XP] <= 0)
                _recover_stat();
        }

        _recharge_xp_evokers(exp_gained);
        _reduce_abyss_xp_timer(exp_gained);

        if (you.attribute[ATTR_XP_DRAIN])
        {
            int loss = div_rand_round(exp_gained * 3 / 2,
                                      calc_skill_cost(you.skill_cost_level));

            // Make it easier to recover from very heavy levels of draining
            // (they're nasty enough as it is)
            loss = loss * (1 + (you.attribute[ATTR_XP_DRAIN] / 250.0f));

            dprf("Lost %d of %d draining points", loss, you.attribute[ATTR_XP_DRAIN]);

            you.attribute[ATTR_XP_DRAIN] -= loss;
            // Regaining skills may affect AC/EV.
            you.redraw_armour_class = true;
            you.redraw_evasion = true;
            if (you.attribute[ATTR_XP_DRAIN] <= 0)
            {
                you.attribute[ATTR_XP_DRAIN] = 0;
                mprf(MSGCH_RECOVERY, "Your life force feels restored.");
            }
        }

        _handle_insight(exp_gained);
        _fade_curses(exp_gained);
    }
}

bool _handle_insight_inv(FixedVector<item_def, 52> *inv)
{// top to bottom
    bool had_insight = false;
    // this give the player the option to move items to the top so that they are more likely to be identified first
    for(item_def &item : *inv)
    {
        if (item.defined()
            && (
                (item.flags & ISFLAG_IDENT_MASK) < ISFLAG_IDENT_MASK)
            || is_deck(item) && !top_card_is_known(item)
            )
        {
            string before, after;
            string before_colored, after_colored;

            if (is_deck(item) && !top_card_is_known(item))
            {
                set_ident_flags(item, ISFLAG_IDENT_MASK);
                set_ident_type(item, true);
                after = get_menu_colour_prefix_tags(item, DESC_A);
                mprf(MSGCH_INTRINSIC_GAIN, "You gain insight into: %s", after.c_str());
                deck_identify_first(item);
                had_insight = true;
                break;
            }
            else
            {
                before = item.name(DESC_A);
                before_colored = get_menu_colour_prefix_tags(item, DESC_A);
                int bitToCheck = 1 << random2(4);
                if((item.flags & bitToCheck) == 0) {
                    set_ident_flags(item, bitToCheck);
                    set_ident_type(item, true);
                    after = item.name(DESC_A);
                    after_colored = get_menu_colour_prefix_tags(item, DESC_A);
                    if(before != after) {
                        mprf(MSGCH_INTRINSIC_GAIN, "You gain insight: %s -> %s", before.c_str(), after.c_str());
                        had_insight = true;
                        break;
                    }
                }
            }
        }
    }

    return had_insight;
}

static void _handle_insight(int exp_gain)
{
    const int skill_cost = calc_skill_cost(you.skill_cost_level);
    const int insight_gained = div_rand_round(exp_gain, skill_cost);
    you.attribute[ATTR_INSIGHT] += insight_gained * 20;

    bool do_more = false;
    while (you.attribute[ATTR_INSIGHT] > 100)
    {
        you.attribute[ATTR_INSIGHT] -= 100;
        
        int lev = player_mutation_level(MUT_INSIGHT);
        if (x_chance_in_y(1 << (lev * 2), 64)) {
            if (_handle_insight_inv(&you.inv1))
                do_more = true;
        }

        if (x_chance_in_y(1 << (lev * 2), 64)) {
            if (_handle_insight_inv(&you.inv2))
                do_more = true;
        }
    }

    if (do_more)
        more();
}

bool player_is_immune_to_curses()
{
    return you.species == SP_MUMMY
           && you.religion != GOD_ASHENZARI;
}

static void _fade_curses(int exp_gained)
{
    for (unsigned int i = 0; i < you.equip.size(); i++)
    {
        int8_t slot = you.equip[i];
        if(slot > -1)
        {
            item_def& item(you.inv1[slot]);
            if(item.cursed())
            {
                if (you.religion != GOD_ASHENZARI)
                {
                    int reduction_amount = 0;
                    const int curseResistance = you.species == SP_DEMIGOD ? 10 : 2;
                    const int howMuchRaw = exp_gained * curseResistance;
                    const int divisor = calc_skill_cost(you.skill_cost_level);
                    reduction_amount = div_rand_round(howMuchRaw, divisor);
                    item.curse_weight = max(0, item.curse_weight - reduction_amount);
                    if (item.curse_weight == 0)
                        mprf("The curse on %s has been lifted!", item.name(DESC_YOUR).c_str());
                }
            }
        }
    }
}

bool will_gain_life(int lev)
{
    if (lev < you.attribute[ATTR_LIFE_GAINED] - 2)
        return false;

    return you.lives + you.deaths < (lev - 1) / 3;
}

static void _felid_extra_life()
{
	return;
    if (will_gain_life(you.max_level)
        && you.lives < 2)
    {
        you.lives++;
        mprf(MSGCH_INTRINSIC_GAIN, "Extra life!");
        you.attribute[ATTR_LIFE_GAINED] = you.max_level;
        // Should play the 1UP sound from SMB...
    }
}

static void _gain_and_note_hp_mp()
{
    const int old_mp = you.mp;
    const int old_maxmp = you.mp_max;

    // recalculate for game
    recalc_and_scale_hp();
    calc_mp();

    set_mp(old_maxmp > 0 ? old_mp * you.mp_max / old_maxmp
           : you.mp_max);

    // Get "real" values for note-taking, i.e. ignore Berserk,
    // transformations or equipped items.
    const int note_maxhp = get_real_hp(false, false);
    const int note_maxmp = get_real_mp(false);
    const int note_maxsp = get_real_sp(false);

    char buf[200];
    if (you.species == SP_DJINNI)
        // Djinn don't HP/MP
        sprintf(buf, "EP: %d/%d",
                min(you.hp, note_maxhp + note_maxmp + note_maxsp),
                note_maxhp + note_maxmp + note_maxsp);
    else
        sprintf(buf, "HP: %d/%d MP: %d/%d",
                min(you.hp, note_maxhp), note_maxhp,
                min(you.mp, note_maxmp), note_maxmp);
    take_note(Note(NOTE_XP_LEVEL_CHANGE, you.experience_level, 0, buf));
}

/**
 * Calculate max HP changes and scale current HP accordingly.
 */
void recalc_and_scale_hp()
{
    // Rounding must be down or Deep Dwarves would abuse certain values.
    // We can reduce errors by a factor of 100 by using partial hp we have.
    int old_max = you.hp_max;
    int hp = you.hp * 100 + you.hit_points_regeneration;
    calc_hp();
    int new_max = you.hp_max;
    hp = hp * new_max / old_max;
    if (hp < 100)
        hp = 100;
    set_hp(min(hp / 100, you.hp_max));
    you.hit_points_regeneration = hp % 100;
}

/**
 * Handle the effects from a player's change in XL.
 * @param aux                     A string describing the cause of the level
 *                                change.
 * @param skip_attribute_increase If true and XL has increased, don't process
 *                                stat gains. Currently only used by wizmode
 *                                commands.
 */
void level_change(bool skip_attribute_increase)
{
    // necessary for the time being, as level_change() is called
    // directly sometimes {dlb}
    you.redraw_experience = true;

    while (you.experience < exp_needed(you.experience_level))
        lose_level();

    while (you.experience_level < you.get_max_xl()
           && you.experience >= exp_needed(you.experience_level + 1))
    {
        if (!skip_attribute_increase)
        {
            crawl_state.cancel_cmd_all();

            if (is_processing_macro())
                flush_input_buffer(FLUSH_ABORT_MACRO);
        }

        // [ds] Make sure we increment you.experience_level and apply
        // any stat/hp increases only after we've cleared all prompts
        // for this experience level. If we do part of the work before
        // the prompt, and a player on telnet gets disconnected, the
        // SIGHUP will save Crawl in the in-between state and rob the
        // player of their level-up perks.

        const int new_exp = you.experience_level + 1;
        // some species need to do this at a specific time; most just do it at the end
        bool updated_maxhp = false;

        if (new_exp <= you.max_level)
        {
            mprf(MSGCH_INTRINSIC_GAIN,
                 "Welcome back to level %d!", new_exp);

            // No more prompts for this XL past this point.

            you.experience_level = new_exp;
        }
        else  // Character has gained a new level
        {
            // Don't want to see the dead creature at the prompt.
            redraw_screen();

            if (new_exp == get_max_exp_level() || Options.level_27_cap && new_exp == 27)
                mprf(MSGCH_INTRINSIC_GAIN, "You have reached level 27, the final one!");
            else if (new_exp == you.get_max_xl())
                mprf(MSGCH_INTRINSIC_GAIN, "You have reached level %d, the highest you will ever reach!",
                        you.get_max_xl());
            else
            {
                mprf(MSGCH_INTRINSIC_GAIN, "You have reached level %d!",
                     new_exp);
            }

            const bool manual_stat_level = new_exp % 3 == 0;  // 3,6,9,12...

            // Must do this before actually changing experience_level,
            // so we will re-prompt on load if a hup is received.
            if (manual_stat_level && !skip_attribute_increase)
                if (!attribute_increase())
                    return; // abort level gain, the xp is still there

            // Set this after printing, since a more() might clear it.
            you.redraw_experience = true;

            crawl_state.stat_gain_prompt = false;
            you.experience_level = new_exp;
            you.max_level = you.experience_level;

#ifdef USE_TILE_LOCAL
            // In case of intrinsic ability changes.
            tiles.layout_statcol();
            redraw_screen();
#endif
            if (!skip_attribute_increase)
                species_stat_gain(you.species);

            switch (you.species)
            {
            case SP_VAMPIRE:
                if (you.experience_level == 3)
                {
                    if (you.hunger_state > HS_SATIATED)
                    {
                        mprf(MSGCH_INTRINSIC_GAIN, "If you weren't so full, "
                             "you could now transform into a vampire bat.");
                    }
                    else
                    {
                        mprf(MSGCH_INTRINSIC_GAIN,
                             "You can now transform into a vampire bat.");
                    }
                }
                break;

            case SP_NAGA:
                if (!(you.experience_level % 3))
                {
                    mprf(MSGCH_INTRINSIC_GAIN, "Your skin feels tougher.");
                    you.redraw_armour_class = true;
                }
                break;

            case SP_BASE_DRACONIAN:
                if (you.experience_level >= 7)
                {
                    you.species = static_cast<species_type>(
                                       random_range(SP_FIRST_NONBASE_DRACONIAN,
                                                    SP_LAST_NONBASE_DRACONIAN));

                    // We just changed our aptitudes, so some skills may now
                    // be at the wrong level (with negative progress); if we
                    // print anything in this condition, we might trigger a
                    // --More--, a redraw, and a crash (#6376 on Mantis).
                    //
                    // Hence we first fix up our skill levels silently (passing
                    // do_level_up = false) but save the old values; then when
                    // we want the messages later, we restore the old skill
                    // levels and call check_skill_level_change() again, this
                    // time passing do_update = true.

                    uint8_t saved_skills[NUM_SKILLS];
                    for (skill_type sk = SK_FIRST_SKILL; sk < NUM_SKILLS; ++sk)
                    {
                        saved_skills[sk] = you.skills[sk];
                        check_skill_level_change(sk, false);
                    }
                    // The player symbol depends on species.
                    update_player_symbol();
#ifdef USE_TILE
                    init_player_doll();
#endif
                    mprf(MSGCH_INTRINSIC_GAIN,
                         "Your scales start taking on %s colour.",
                         article_a(scale_type(you.species)).c_str());

                    // Produce messages about skill increases/decreases. We
                    // restore one skill level at a time so that at most the
                    // skill being checked is at the wrong level.
                    for (skill_type sk = SK_FIRST_SKILL; sk < NUM_SKILLS; ++sk)
                    {
                        const int oldapt = species_apt(sk, SP_BASE_DRACONIAN);
                        const int newapt = species_apt(sk, you.species);
                        if (oldapt != newapt)
                        {
                            mprf(MSGCH_INTRINSIC_GAIN, "You learn %s %s%s.",
                                 skill_name(sk),
                                 abs(oldapt - newapt) > 1 ? "much " : "",
                                 oldapt > newapt ? "slower" : "quicker");
                        }

                        you.skills[sk] = saved_skills[sk];
                        check_skill_level_change(sk);
                    }

                    // Tell the player about their new species
                    for (auto &mut : fake_mutations(you.species, false))
                        mprf(MSGCH_INTRINSIC_GAIN, "%s", mut.c_str());

                    // needs to be done early here, so HP doesn't look rotted
                    // when we redraw the screen
                    _gain_and_note_hp_mp();
                    updated_maxhp = true;

                    redraw_screen();
                }
                break;

            case SP_DEMONSPAWN:
            {
                bool gave_message = false;
                int level = 0;
                mutation_type first_body_facet = NUM_MUTATIONS;

                for (const player::demon_trait trait : you.demonic_traits)
                {
                    if (is_body_facet(trait.mutation))
                    {
                        if (first_body_facet < NUM_MUTATIONS
                            && trait.mutation != first_body_facet)
                        {
                            if (you.experience_level == level)
                            {
                                mprf(MSGCH_MUTATION, "You feel monstrous as "
                                     "your demonic heritage exerts itself.");
                                mark_milestone("monstrous", "discovered their "
                                               "monstrous ancestry!");
                            }
                            break;
                        }

                        if (first_body_facet == NUM_MUTATIONS)
                        {
                            first_body_facet = trait.mutation;
                            level = trait.level_gained;
                        }
                    }
                }

                for (const player::demon_trait trait : you.demonic_traits)
                {
                    if (trait.level_gained == you.experience_level)
                    {
                        if (!gave_message)
                        {
                            mprf(MSGCH_INTRINSIC_GAIN,
                                 "Your demonic ancestry asserts itself...");

                            gave_message = true;
                        }
                        perma_mutate(trait.mutation, 1, "demonic ancestry");
                    }
                }

                break;
            }

            case SP_FELID:
                _felid_extra_life();
                break;

            default:
                break;
            }

            give_level_mutations(you.species, you.experience_level);

        }

        if (species_is_draconian(you.species) && !(you.experience_level % 3))
        {
            mprf(MSGCH_INTRINSIC_GAIN, "Your scales feel tougher.");
            you.redraw_armour_class = true;
        }
        if (!updated_maxhp)
            _gain_and_note_hp_mp();

        xom_is_stimulated(12);
        if (in_good_standing(GOD_HEPLIAKLQANA))
        {
            upgrade_hepliaklqana_ancestor();
            if (you.experience_level == HEP_SPECIALIZATION_LEVEL
                && you.props.exists(HEPLIAKLQANA_ALLY_TYPE_KEY))
            {
                god_speaks(you.religion,
                           "You may now specialize your ancestor.");
            }
        }

        learned_something_new(HINT_NEW_LEVEL);
    }

    while (you.experience >= exp_needed(you.max_level + 1))
    {
        ASSERT(you.experience_level == you.get_max_xl());
        ASSERT(you.max_level < 127); // marshalled as an 1-byte value
        you.max_level++;
        if (you.species == SP_FELID)
            _felid_extra_life();
    }

    you.redraw_title = true;

#ifdef DGL_WHEREIS
    whereis_record();
#endif

    // Hints mode arbitrarily ends at xp 7.
    if (crawl_state.game_is_hints() && you.experience_level >= 7)
        hints_finished();
}

void adjust_level(int diff, bool just_xp)
{
    ASSERT((uint64_t)you.experience <= (uint64_t)MAX_EXP_TOTAL);
    const int max_exp_level = you.get_max_xl();
    if (you.experience_level + diff < 1)
        you.experience = 0;
    else if (you.experience_level + diff >= max_exp_level)
    {
        const unsigned needed = exp_needed(max_exp_level);
        // Level gain when already at max should never reduce player XP;
        // but level loss (diff < 0) should.
        if (diff < 0 || you.experience < needed)
            you.experience = needed;
    }
    else
    {
        while (diff < 0 && you.experience >=
                exp_needed(max_exp_level))
        {
            // Having XP for level 53 and going back to 26 due to a single
            // card would mean your felid is not going to get any extra lives
            // in foreseable future.
            you.experience -= exp_needed(max_exp_level)
                    - exp_needed(max_exp_level - 1);
            diff++;
        }
        int old_min = exp_needed(you.experience_level);
        int old_max = exp_needed(you.experience_level + 1);
        int new_min = exp_needed(you.experience_level + diff);
        int new_max = exp_needed(you.experience_level + 1 + diff);
        dprf("XP before: %d\n", you.experience);
        dprf("%4.2f of %d..%d to %d..%d",
             (you.experience - old_min) * 1.0 / (old_max - old_min),
             old_min, old_max, new_min, new_max);

        you.experience = ((int64_t)(new_max - new_min))
                       * (you.experience - old_min)
                       / (old_max - old_min)
                       + new_min;
        dprf("XP after: %d\n", you.experience);
    }

    ASSERT((uint64_t)you.experience <= (uint64_t)MAX_EXP_TOTAL);

    if (!just_xp)
        level_change();
}

/**
 * Return a multiplier for skill when calculating stealth values, based on the
 * player's species & form.
 *
 * @return The player's current stealth multiplier value.
 */
static int _stealth_mod()
{
    const int form_stealth_mod = get_form()->get_stealth_mod();

    if (form_stealth_mod != 0)
        return form_stealth_mod;

    int species_stealth_mod = species_stealth_modifier(you.species);
    if (you.form == TRAN_STATUE)
        species_stealth_mod -= 3;
    // Thirsty vampires are (much) more stealthy
    if (you.species == SP_VAMPIRE)
    {
        switch (you.hunger_state)
        {
        case HS_FAINTING:
        case HS_STARVING:
            species_stealth_mod += 3;
            break;

        case HS_NEAR_STARVING:
            species_stealth_mod += 2;
            break;

        case HS_VERY_HUNGRY:
        case HS_HUNGRY:
            species_stealth_mod += 1;
            break;
        default:
            break;
        }
    }
    return species_stealth_mod;
}

/**
 * Get the player's current stealth value.
 *
 * XXX: rename this to something more reasonable

 *
 * (Keep in mind, while tweaking this function: the order in which stealth
 * modifiers are applied is significant!)
 *
 * @return  The player's current stealth value.
 */
int check_stealth()
{
    ASSERT(!crawl_state.game_is_arena());
    // Extreme stealthiness can be enforced by wizmode stealth setting.
    if (crawl_state.disables[DIS_MON_SIGHT])
        return 1000;

    // lantern of shadows, berserking, "clumsy" (0-dex).
    if (you.attribute[ATTR_SHADOWS] || you.berserk()
        || you.duration[DUR_CLUMSY] || player_mutation_level(MUT_NO_STEALTH))
    {
        return 0;
    }

    int stealth = you.dex() * 3;

    if (you.form == TRAN_BLADE_HANDS && you.species == SP_FELID
        && !you.airborne())
    {
        stealth -= 50; // klack klack klack go the blade paws
        // this is an absurd special case but also it's really funny so w/e
    }

    stealth += you.skill(SK_STEALTH, _stealth_mod());

    if (you.confused())
        stealth /= 3;

    const item_def *arm = you.slot_item(EQ_BODY_ARMOUR, false);
    const item_def *boots = you.slot_item(EQ_BOOTS, false);

    if (arm)
    {
        // [ds] New stealth penalty formula from rob: SP = 6 * (EP^2)
        // Now 2 * EP^2 / 3 after EP rescaling.
        const int evp = you.unadjusted_body_armour_penalty();
        const int penalty = evp * evp * 2 / 3;
#if 0
        dprf("Stealth penalty for armour (ep: %d): %d", ep, penalty);
#endif
        stealth -= penalty;
    }

    stealth += STEALTH_PIP * you.scan_artefacts(ARTP_STEALTH);

    stealth += STEALTH_PIP * you.wearing(EQ_RINGS, RING_STEALTH);
    stealth -= STEALTH_PIP * you.wearing(EQ_RINGS, RING_LOUDNESS);

    const item_def *body_armour = you.slot_item(EQ_BODY_ARMOUR);
    if (body_armour)
    {
        const int pips = armour_type_prop(body_armour->sub_type, ARMF_STEALTH);
        stealth += pips * STEALTH_PIP;
    }

    if (you.duration[DUR_STEALTH])
        stealth += STEALTH_PIP * 2;

    if (you.duration[DUR_AGILITY])
        stealth += STEALTH_PIP;

    if (!you.airborne())
    {
        if (you.in_water())
        {
            // Merfolk can sneak up on monsters underwater -- bwr
            if (you.fishtail || you.species == SP_OCTOPODE)
                stealth += STEALTH_PIP;
            else if (!you.can_swim() && !you.extra_balanced())
                stealth /= 2;       // splashy-splashy
        }

        else if (boots && get_armour_ego_type(*boots) == SPARM_STEALTH)
            stealth += STEALTH_PIP;

        else if (you.has_usable_hooves())
            stealth -= 5 + 5 * player_mutation_level(MUT_HOOVES);

        else if (you.species == SP_FELID && (!you.form || you.form == TRAN_APPENDAGE))
            stealth += 20;  // paws
    }

    // Radiating silence is the negative complement of shouting all the
    // time... a sudden change from background noise to no noise is going
    // to clue anything in to the fact that something is very wrong...
    // a personal silence spell would naturally be different, but this
    // silence radiates for a distance and prevents monster spellcasting,
    // which pretty much gives away the stealth game.
    // this penalty is dependent on the actual amount of ambient noise
    // in the level -doy
    if (you.duration[DUR_SILENCE])
        stealth -= STEALTH_PIP + current_level_ambient_noise();

    // Mutations.
    stealth += STEALTH_PIP * player_mutation_level(MUT_NIGHTSTALKER);
    stealth += (STEALTH_PIP / 2)
                * player_mutation_level(MUT_THIN_SKELETAL_STRUCTURE);

    if (player_mutation_level(MUT_GLOW) > 0)
    	stealth -= STEALTH_PIP / 2;
    if (player_mutation_level(MUT_GLOW) > 1)
    	stealth -= STEALTH_PIP;
    if (player_mutation_level(MUT_GLOW) > 2)
    	stealth -= STEALTH_PIP * 2;

    stealth += STEALTH_PIP * player_mutation_level(MUT_CAMOUFLAGE);
    const int how_transparent = player_mutation_level(MUT_TRANSLUCENT_SKIN);
    if (how_transparent)
        stealth += 15 * (how_transparent);

    // it's easier to be stealthy when there's a lot of background noise
    stealth += 2 * current_level_ambient_noise();

    // If you've been tagged with Corona or are Glowing, the glow
    // makes you extremely unstealthy.
    if (you.backlit())
        stealth = stealth * 2 / 5;

    // On the other hand, shrouding has the reverse effect, if you know
    // how to make use of it:
    if (you.umbra())
    {
        int umbra_mul = 1, umbra_div = 1;
        if (you.nightvision())
        {
            umbra_mul = you.piety + MAX_PIETY;
            umbra_div = MAX_PIETY;
        }
        if (player_equip_unrand(UNRAND_SHADOWS)
            && 2 * umbra_mul < 3 * umbra_div)
        {
            umbra_mul = 3;
            umbra_div = 2;
        }
        stealth *= umbra_mul;
        stealth /= umbra_div;
    }

    // If you're surrounded by a storm, you're inherently pretty conspicuous.
    if (have_passive(passive_t::storm_shield))
    {
        stealth = stealth
                  * (MAX_PIETY - min((int)you.piety, piety_breakpoint(5)))
                  / (MAX_PIETY - piety_breakpoint(0));
    }
    // The shifting glow from the Orb, while too unstable to negate invis
    // or affect to-hit, affects stealth even more than regular glow.
    if (player_has_orb())
        stealth /= 3;

    stealth = player_stealth_modifier(stealth);
    stealth = max(0, stealth);

    return stealth;
}

// Is a given duration about to expire?
bool dur_expiring(duration_type dur)
{
    const int value = you.duration[dur];
    if (value <= 0)
        return false;

    return value <= duration_expire_point(dur);
}

static void _output_expiring_message(duration_type dur, const char* msg)
{
    if (you.duration[dur])
    {
        const bool expires = dur_expiring(dur);
        mprf("%s%s", expires ? "Expiring: " : "", msg);
    }
}

static void _display_char_status(int value, const char *fmt, ...)
{
    va_list argp;
    va_start(argp, fmt);

    string msg = vmake_stringf(fmt, argp);

    if (you.wizard)
        mprf("%s (%d).", msg.c_str(), value);
    else
        mprf("%s.", msg.c_str());

    va_end(argp);
}

static void _display_vampire_status()
{
    string msg = "At your current hunger state you ";
    vector<const char *> attrib;

    switch (you.hunger_state)
    {
        case HS_FAINTING:
        case HS_STARVING:
            attrib.push_back("are immune to poison");
            attrib.push_back("significantly resist cold");
            attrib.push_back("strongly resist negative energy");
            attrib.push_back("resist torment");
            attrib.push_back("do not heal.");
            break;
        case HS_NEAR_STARVING:
            attrib.push_back("resist poison");
            attrib.push_back("significantly resist cold");
            attrib.push_back("strongly resist negative energy");
            attrib.push_back("have an extremely slow metabolism");
            attrib.push_back("heal slowly.");
            break;
        case HS_VERY_HUNGRY:
        case HS_HUNGRY:
            attrib.push_back("resist poison");
            attrib.push_back("resist cold");
            attrib.push_back("significantly resist negative energy");
            if (you.hunger_state == HS_HUNGRY)
                attrib.push_back("have a slow metabolism");
            else
                attrib.push_back("have a very slow metabolism");
            attrib.push_back("heal slowly.");
            break;
        case HS_SATIATED:
            attrib.push_back("resist negative energy.");
            break;
        case HS_FULL:
            attrib.push_back("have a fast metabolism");
            attrib.push_back("heal quickly.");
            break;
        case HS_VERY_FULL:
            attrib.push_back("have a very fast metabolism");
            attrib.push_back("heal quickly.");
            break;
        case HS_ENGORGED:
            attrib.push_back("have an extremely fast metabolism");
            attrib.push_back("heal extremely quickly.");
            break;
    }

    if (!attrib.empty())
    {
        msg += comma_separated_line(attrib.begin(), attrib.end());
        mpr(msg);
    }
}

static void _display_movement_speed()
{
    const int move_cost = (player_speed() * player_movement_speed()) / 10;

    const bool water  = you.in_liquid();
    const bool swim   = you.swimming();

    const bool fly    = you.airborne();
    const bool swift  = (you.duration[DUR_SWIFTNESS] > 0
                         && you.attribute[ATTR_SWIFTNESS] >= 0);
    const bool antiswift = (you.duration[DUR_SWIFTNESS] > 0
                            && you.attribute[ATTR_SWIFTNESS] < 0);

    _display_char_status(move_cost, "Your %s speed is %s%s%s",
          // order is important for these:
          (swim)    ? "swimming" :
          (water)   ? "wading" :
          (fly)     ? "flying"
                    : "movement",

          (water && !swim)  ? "uncertain and " :
          (!water && swift) ? "aided by the wind" :
          (!water && antiswift) ? "hindered by the wind" : "",

          (!water && swift) ? ((move_cost >= 10) ? ", but still "
                                                 : " and ") :
          (!water && antiswift) ? ((move_cost <= 10) ? ", but still "
                                                     : " and ")
                            : "",

          (move_cost <   8) ? "very quick" :
          (move_cost <  10) ? "quick" :
          (move_cost == 10) ? "average" :
          (move_cost <  13) ? "slow"
                            : "very slow");
}

static void _display_tohit()
{
    melee_attack attk(&you, nullptr);
    const int to_hit = attk.calc_to_hit();
    mprf("To-hit: %d", to_hit);
}

static const char* _attack_delay_desc(int attack_delay)
{
    return (attack_delay >= 200) ? "extremely slow" :
           (attack_delay >= 155) ? "very slow" :
           (attack_delay >= 125) ? "quite slow" :
           (attack_delay >= 105) ? "below average" :
           (attack_delay >=  95) ? "average" :
           (attack_delay >=  75) ? "above average" :
           (attack_delay >=  55) ? "quite fast" :
           (attack_delay >=  45) ? "very fast" :
           (attack_delay >=  35) ? "extremely fast" :
                                   "blindingly fast";
}

/**
 * Print a message indicating the player's attack delay with their current
 * weapon & its ammo (if applicable).
 *
 * Assumes the attack speed of a ranged weapon does not depend on what
 * ammunition is being used (as long as it is valid).
 */
static void _display_attack_delay()
{
    const item_def* weapon = you.weapon();
    int delay;
    if (weapon && is_range_weapon(*weapon))
    {
        item_def ammo;
        ammo.base_type = OBJ_MISSILES;
        ammo.sub_type = fires_ammo_type(*weapon);
        delay = you.attack_delay(&ammo, false);
    }
    else
        delay = you.attack_delay(nullptr, false);

    const bool at_min_delay = weapon
                              && you.skill(item_attack_skill(*weapon))
                                 >= weapon_min_delay_skill(*weapon);

    // Scale to fit the displayed weapon base delay, i.e.,
    // normal speed is 100 (as in 100%).
    int avg = 10 * delay;

    _display_char_status(avg, "Your attack speed is %s%s%s",
                         _attack_delay_desc(avg),
                         at_min_delay ?
                            " (and cannot be improved with additional weapon skill)" : "",
                         you.adjusted_shield_penalty() ?
                            " (and is slowed by your insufficient shield skill)" : "");
}

// forward declaration
static string _constriction_description();

void display_char_status()
{
    if (you.undead_state() == US_SEMI_UNDEAD
        && you.hunger_state == HS_ENGORGED)
    {
        mpr("You feel almost alive.");
    }
    else if (you.undead_state())
        mpr("You are undead.");
    else if (you.duration[DUR_DEATHS_DOOR])
    {
        _output_expiring_message(DUR_DEATHS_DOOR,
                                 "You are standing in death's doorway.");
    }
    else
        mpr("You are alive.");

    const int halo_size = you.halo_radius();
    if (halo_size >= 0)
    {
        if (halo_size > 37)
            mpr("You are illuminated by a large divine halo.");
        else if (halo_size > 10)
            mpr("You are illuminated by a divine halo.");
        else
            mpr("You are illuminated by a small divine halo.");
    }
    else if (you.haloed())
        mpr("An external divine halo illuminates you.");

    if (you.species == SP_VAMPIRE)
        _display_vampire_status();

    status_info inf;
    for (unsigned i = 0; i <= STATUS_LAST_STATUS; ++i)
    {
        if (fill_status_info(i, &inf) && !inf.long_text.empty())
            mpr(inf.long_text);
    }
    string cinfo = _constriction_description();
    if (!cinfo.empty())
        mpr(cinfo);

    _display_movement_speed();
    _display_tohit();
    _display_attack_delay();

    // Display base attributes, if necessary.
    if (innate_stat(STAT_STR) != you.strength()
        || innate_stat(STAT_INT) != you.intel()
        || innate_stat(STAT_DEX) != you.dex())
    {
        mprf("Your base attributes are Str %d, Int %d, Dex %d.",
             innate_stat(STAT_STR),
             innate_stat(STAT_INT),
             innate_stat(STAT_DEX));
    }

    // magic resistance
    _display_char_status(you.res_magic(),
                         "You are %s to hostile enchantments",
                         magic_res_adjective(player_res_magic(false)).c_str());

    // character evaluates their ability to sneak around:
    _display_char_status(check_stealth(),
                         "You are %s",
                         stealth_desc(check_stealth()).c_str());
}

bool player::clarity(bool calc_unid, bool items) const
{
    if (player_mutation_level(MUT_CLARITY))
        return true;

    if (have_passive(passive_t::clarity))
        return true;

    return actor::clarity(calc_unid, items);
}

bool player::gourmand(bool calc_unid, bool items) const
{
    return player_mutation_level(MUT_GOURMAND) > 0
           || actor::gourmand(calc_unid, items);
}

int player::spec_evoke(bool calc_unid, bool items) const
{
    return actor::spec_evoke(calc_unid, items)
           + attribute[ATTR_PAKELLAS_DEVICE_SURGE];
}

bool player::stasis(bool calc_unid, bool items) const
{
    return species == SP_FORMICID;
}

unsigned int exp_needed(int lev, int exp_apt)
{
    if (lev <= 1)
        return 0;

    if (exp_apt == -99)
        exp_apt = species_exp_modifier(you.species);

    int needed_exp;
    if (Options.level_27_cap)
    {
        unsigned int level = 0;

//         Note: For historical reasons, all of the following numbers are for a
//         species (like human) with XP aptitude 1, not 0 as one might expect.
//
//         Basic plan:
//         Section 1: levels  1- 5, second derivative goes 10-10-20-30.
//         Section 2: levels  6-13, second derivative is exponential/doubling.
//         Section 3: levels 14-27, second derivative is constant at 8470.
//
//         Here's a table:

//         level      xp      delta   delta2
//         =====   =======    =====   ======
//           1           0        0       0
//           2          10       10      10
//           3          30       20      10
//           4          70       40      20
//           5         140       70      30
//           6         270      130      60
//           7         520      250     120
//           8        1010      490     240
//           9        1980      970     480
//          10        3910     1930     960
//          11        7760     3850    1920
//          12       15450     7690    3840
//          13       26895    11445    3755
//          14       45585    18690    7245
//          15       72745    27160    8470
//          16      108375    35630    8470
//          17      152475    44100    8470
//          18      205045    52570    8470
//          19      266085    61040    8470
//          20      335595    69510    8470
//          21      413575    77980    8470
//          22      500025    86450    8470
//          23      594945    94920    8470
//          24      698335    103390   8470
//          25      810195    111860   8470
//          26      930525    120330   8470
//          27     1059325    128800   8470

        switch (lev)
        {
            case 1:
                level = 1;
                break;
            case 2:
                level = 10;
                break;
            case 3:
                level = 30;
                break;
            case 4:
                level = 70;
                break;

            default:
                if (lev < 13)
                {
                    lev -= 4;
                    level = 10 + 10 * lev + (60 << lev);
                }
                else
                {
                    lev -= 12;
                    level = 16675 + 5985 * lev + 4235 * lev * lev;
                }
                break;
        }

        const float apt_factor = apt_to_factor(exp_apt - 1);
        needed_exp = (level - 1) * apt_factor * apt_factor * apt_factor;
    }
    else
    {
        const float apt_factor = apt_to_factor(exp_apt - 1);
        const int base = stepup2(lev + 1, 4, 6, 10) + 10;
        needed_exp = base * apt_factor * apt_factor * apt_factor;
    }
    return (unsigned int) needed_exp;
}

// returns bonuses from rings of slaying, etc.
int slaying_bonus(bool ranged)
{
    int ret = 0;

    ret += you.wearing(EQ_RINGS_PLUS, RING_SLAYING);
    ret += you.scan_artefacts(ARTP_SLAYING);
    if (you.wearing_ego(EQ_GLOVES, SPARM_ARCHERY) && ranged)
        ret += 4;

    ret += 4 * augmentation_amount();

    if (you.duration[DUR_SONG_OF_SLAYING])
        ret += you.props[SONG_OF_SLAYING_KEY].get_int();

    if (you.duration[DUR_HORROR])
        ret -= you.props[HORROR_PENALTY_KEY].get_int();

    return ret;
}

// Checks each equip slot for a randart, and adds up all of those with
// a given property. Slow if any randarts are worn, so avoid where
// possible. If `matches' is non-nullptr, items with nonzero property are
// pushed onto *matches.
int player::scan_artefacts(artefact_prop_type which_property,
                           bool calc_unid,
                           vector<item_def> *matches) const
{
    int retval = 0;

    for (int i = EQ_WEAPON; i < NUM_EQUIP; ++i)
    {
        if (melded[i] || equip[i] == -1)
            continue;

        const int eq = equip[i];

        // Only weapons give their effects when in our hands.
        if (i == EQ_WEAPON && inv1[ eq ].base_type != OBJ_WEAPONS)
            continue;

        if (!is_artefact(inv1[ eq ]))
            continue;

        bool known;
        int val = artefact_property(inv1[eq], which_property, known);
        if (calc_unid || known)
        {
            const item_def &item = inv1[eq];
            retval += val;
            if (matches && val && !item.super_cursed())
            {
                matches->push_back(item);
            }
        }
    }

    return retval;
}

void calc_hp()
{
    int oldhp = you.hp, oldmax = you.hp_max;
    you.hp_max = get_real_hp(true, false);

    if (you.duration[DUR_BERSERK])
        you.hp_max = you.hp_max * 3 / 2;

    if (you.species == SP_DJINNI)
    {
        you.hp_max += get_real_mp(true);
        you.hp_max += get_real_sp(true);
    }
    deflate_hp(you.hp_max, false);
    if (oldhp != you.hp || oldmax != you.hp_max)
        dprf("HP changed: %d/%d -> %d/%d", oldhp, oldmax, you.hp, you.hp_max);
    you.redraw_hit_points = true;
}

void calc_sp()
{
    if (you.species == SP_DJINNI)
    {
        you.sp = you.sp_max = 0;
        return calc_hp();
    }

    int oldsp = you.sp, oldmax = you.sp_max;
    you.sp_max = get_real_sp(true);
    if (you.sp > you.sp_max)
        you.sp = you.sp_max;
    if (oldsp != you.sp || oldmax != you.sp_max)
        dprf("SP changed: %d/%d -> %d/%d", oldsp, oldmax, you.sp, you.sp_max);
    you.redraw_stamina_points = true;
}

bool dec_hp(int hp_loss, bool fatal, const char *aux)
{
    ASSERT(!crawl_state.game_is_arena());

    bool result = true;
    if (!fatal && you.hp < 1)
        you.hp = 1;

    if (!fatal && hp_loss >= you.hp)
        hp_loss = you.hp - 1;

    // If it's not fatal, use ouch() so that notes can be taken. If it IS
    // fatal, somebody else is doing the bookkeeping, and we don't want to mess
    // with that.
    if (!fatal && aux)
        ouch(hp_loss, KILLED_BY_SOMETHING, MID_NOBODY, aux);
    else
        you.hp -= hp_loss;

    result = you.hp >= 0;

    if(you.hp > you.hp_max) you.hp = you.hp_max;

    you.redraw_hit_points = true;

    you.peace = 0;

    return result;
}

void calc_mp()
{
    if (you.species == SP_DJINNI)
    {
        you.mp = you.mp_max = 0;
        return calc_hp();
    }

    you.mp_max = get_real_mp(true);
    you.mp = min(you.mp, you.mp_max);
    you.redraw_magic_points = true;
}

void flush_mp()
{
    if (Options.magic_point_warning
        && you.mp < you.mp_max
                              * Options.magic_point_warning / 100)
    {
        mprf(MSGCH_DANGER, "* * * LOW MAGIC WARNING * * *");
    }

    take_note(Note(NOTE_MP_CHANGE, you.mp, you.mp_max));
    you.redraw_magic_points = true;
}

void _handle_overexertion(const int overdraft)
{
    you.duration[DUR_TIRED] = 1;

    mprf(MSGCH_WARN, "You are pushing your body beyond it's limits!");

    int reduction = 50;
    if (you.rune_curse_active[RUNE_SWAMP])
        reduction = 10;

    const int rot_amount = div_rand_round(overdraft, reduction);
    if (rot_amount)
    {
        mprf(MSGCH_WARN, "Your body breaks down under the strain! (rot+%d)", rot_amount);
        rot_hp(rot_amount);
    }
}

// returns false if there isn't enough mp
bool dec_mp(int mp_loss, bool silent, bool allow_overdrive)
{
    ASSERT(!crawl_state.game_is_arena());
    bool result = true;

    if (mp_loss < 1)
        return true;

    if (you.species == SP_DJINNI)
        dec_hp(mp_loss, false);
    else
        you.mp -= mp_loss;

    if (you.mp < 0 || you.species == SP_DJINNI && you.hp < you.hp_max * 25 / 100)
    {
        const int overdraft = -you.mp;
        if (allow_overdrive)
            _handle_overexertion(overdraft);

        you.mp = max(0, you.mp);
        bool sent_message = false;
        result = false;

        if (you.exertion == EXERT_FOCUS && !sent_message && !silent)
        {
            mpr("Your energy is too low to continue exerting yourself.");
            sent_message = true;
        }

        if (you.exertion != EXERT_NORMAL)
        {
            set_exertion(EXERT_NORMAL, false);
        }
    }

    if (!silent)
        flush_mp();

    return result;
}

bool drain_mp(int loss)
{
    bool result = true;
    if (you.species == SP_DJINNI)
    {

        if (loss <= 0)
            return true;

        you.duration[DUR_ANTIMAGIC] = min(you.duration[DUR_ANTIMAGIC] + loss * 3,
                                           1000); // so it goes away after one '5'
    }
    else
        result = dec_mp(loss);

    return result;
}

bool enough_hp(int minimum, bool suppress_msg, bool abort_macros)
{
    ASSERT(!crawl_state.game_is_arena());

    if (you.duration[DUR_DEATHS_DOOR])
    {
        if (!suppress_msg)
            mpr("You cannot pay life while functionally dead.");

        if (abort_macros)
        {
            crawl_state.cancel_cmd_again();
            crawl_state.cancel_cmd_repeat();
        }
        return false;
    }

    // We want to at least keep 1 HP. -- bwr
    if (you.hp < minimum + 1)
    {
        if (!suppress_msg)
            mpr("You don't have enough health at the moment.");

        if (abort_macros)
        {
            crawl_state.cancel_cmd_again();
            crawl_state.cancel_cmd_repeat();
        }
        return false;
    }

    return true;
}

bool enough_mp(int minimum, bool suppress_msg, bool abort_macros)
{
    if (you.species == SP_DJINNI)
        return enough_hp(minimum * 1, suppress_msg);

    ASSERT(!crawl_state.game_is_arena());

    if (you.mp < minimum)
    {
        if (!suppress_msg)
        {
            if (get_real_mp(true) < minimum)
                mpr("You don't have enough magic capacity.");
            else
                mpr("You don't have enough magic at the moment.");
        }
        if (abort_macros)
        {
            crawl_state.cancel_cmd_again();
            crawl_state.cancel_cmd_repeat();
        }
        return false;
    }

    return true;
}

static int _rest_trigger_level(int max)
{
    return (max * Options.rest_wait_percent) / 100;
}

static bool _should_stop_resting(int cur, int max)
{
    return cur == max || cur == _rest_trigger_level(max);
}

int get_hp()
{
    return you.hp;
}

int get_hp_max()
{
    return you.hp_max;
}

int get_sp()
{
    if (you.species == SP_DJINNI)
        return you.hp;
    return you.sp;
}

int get_sp_max(bool raw)
{
    if (you.species == SP_DJINNI)
        return you.hp_max;
    return you.sp_max;
}

int get_mp()
{
    if (you.species == SP_DJINNI)
        return you.hp;
    return you.mp;
}

int get_mp_max(bool raw)
{
    if (you.species == SP_DJINNI)
        return you.hp_max;
    int max = you.mp_max;
    if (raw)
        max += you.mp_frozen_summons;

    return max;
}

bool player_is_very_tired(bool silent)
{
    if (Options.exertion_disabled)
        return false;

    const bool is_tired = player_sp_is_exhausted(true) || player_mp_is_exhausted(true);
    /* old way
    const bool is_tired = you.duration[DUR_EXHAUSTED] > 0;
     */

    if (!silent && is_tired)
        mpr("Your are exhausted!");

    return is_tired;
}

bool player_mp_is_exhausted(bool silent)
{
    const bool is_tired = get_mp() < 5;

    if (!silent && is_tired)
        mpr("Your energy is low!");

    return is_tired;
}

/* used to give stamina penalties such as lower melee / ranged damage */
bool player_sp_is_exhausted(bool silent)
{
    const bool is_tired = get_sp() < 5;

    if (!silent && is_tired)
        mpr("You are exhausted!");

    return is_tired;
}

bool in_quick_mode()
{
    return you.stamina_flags & STAMF_QUICK_MODE;
}

void set_quick_mode(const bool new_quick_mode, const bool manual)
{
    if (crawl_state.sim_mode)
        return;

    if (new_quick_mode == in_quick_mode())
        return;

    if (you.religion == GOD_CHEIBRIADOS && new_quick_mode)
    {
        mprf("Cheibriados prevents you from entering quick mode.");
        return;
    }

    if (manual)
    {
        if (new_quick_mode)
            you.stamina_flags |= STAMF_QUICK_MODE_RESTORE;
        else
            you.stamina_flags &= ~STAMF_QUICK_MODE_RESTORE;
    }

    if (new_quick_mode)
    {
        you.stamina_flags |= STAMF_SKIP_MOVEMENT_PENALTY;
        you.stamina_flags |= STAMF_QUICK_MODE;
        you.duration[DUR_QUICK_MODE] = 1;
    }
    else
    {
        you.stamina_flags &= ~STAMF_SKIP_MOVEMENT_PENALTY;
        you.stamina_flags &= ~STAMF_QUICK_MODE;
        you.duration[DUR_QUICK_MODE] = 0;
        /*
        you.turn_is_over = true;
         */
    }

    you.redraw_status_lights = true;
    you.redraw_evasion = true;
}

void set_exertion(const exertion_mode new_exertion, const bool manual)
{
    if (crawl_state.sim_mode)
        return;

    if (Options.exertion_disabled)
    {
        if (manual)
            mpr("Exertion modes are disabled in your rc file, so you can't use power or focus modes.");
        return;
    }

    if (manual)
        you.restore_exertion = new_exertion;

    if (new_exertion == you.exertion)
        return;

    if (you.duration[DUR_BERSERK])
    {
        if (manual)
            mpr("You can't change exertion mode while berserk.");
        return;
    }

    if (player_is_very_tired(true) && new_exertion != EXERT_NORMAL)
    {
        if (manual)
            mpr("You are too tired to exert yourself now.");
        return;
    }

    you.exertion = new_exertion;
    you.duration[DUR_FOCUS_MODE] = 0;
    you.duration[DUR_POWER_MODE] = 0;
    switch(new_exertion)
    {
        case EXERT_POWER:
            you.duration[DUR_POWER_MODE] = 1;
            break;
        case EXERT_FOCUS:
            you.duration[DUR_FOCUS_MODE] = 1;
            break;
        default:
            break;
    }
    you.redraw_status_lights = true;
    you.redraw_evasion = true;
    you.redraw_armour_class = true;
    /*
    you.turn_is_over = true;
     */
}

// returns true if after subtracting the given sp, sp is still > 0
bool dec_sp(int sp_loss, bool silent, bool allow_overdrive)
{
    bool result = true;

    if (you.duration[DUR_TIRELESS])
        sp_loss = div_rand_round(sp_loss, 4);

    if (sp_loss < 1)
        return true;

    const int fast_metabolism = player_mutation_level(MUT_FAST_METABOLISM);
    if (fast_metabolism)
        sp_loss = qpow(sp_loss, 4, 3, fast_metabolism);

    const int slow_metabolism = player_mutation_level(MUT_SLOW_METABOLISM);
    if (slow_metabolism)
    {
        sp_loss = qpow(sp_loss, 3, 4, slow_metabolism);
        sp_loss = max(1, sp_loss);
    }

    if (you.species == SP_DJINNI)
        dec_hp(sp_loss, false);
    else
        you.sp -= sp_loss;

    if (you.sp < 0 || you.species == SP_DJINNI && you.hp < you.hp_max * 25 / 100)
    {
        const int overdraft = -you.sp;
        if (allow_overdrive)
            _handle_overexertion(overdraft);

        you.sp = 0;

        if (you.duration[DUR_BERSERK] > 1 && you.duration_source[DUR_BERSERK] != SRC_POTION)
        {
            if (!silent)
                mpr("You are too tired to continue your rampage.");
            you.duration[DUR_BERSERK] = 1;
        }

        if (you.duration[DUR_HASTE] > 0 && you.duration_source[DUR_HASTE] != SRC_POTION)
        {
            if (!silent)
                mpr("You are too tired to maintain this pace.");
            you.duration[DUR_HASTE] = 0;
        }

        if (you.duration[DUR_INVIS] > 0 && you.duration_source[DUR_INVIS] != SRC_POTION)
        {
            if (!silent)
                mpr("You are too tired to stay invisible.");
            you.duration[DUR_INVIS] = 0;
        }

        if (you.digging)
        {
            you.digging = false;
            if (!silent)
                mpr("You are too tired to continue digging.");
        }

        if (you.exertion != EXERT_NORMAL)
        {
            if (!silent)
                mpr("You are too tired to continue exerting yourself.");
            set_exertion(EXERT_NORMAL, false);
        }

        if (in_quick_mode())
        {
            if (!silent)
                mpr("You are too tired to continue at this pace.");
            set_quick_mode(false, false);
        }

        result = false;
        you.redraw_evasion = true;
        you.redraw_armour_class = true;
        you.redraw_hit_chance = true;
    }

    you.redraw_stamina_points = true;

    return result;
}

void _restore_exertion_mode()
{
    if (!player_is_very_tired(true))
        you.duration[DUR_TIRED] = 0;

    const int sp_target = get_sp_max() / 2;
    if (get_sp() >= sp_target)
    {
        if (you.stamina_flags & STAMF_QUICK_MODE_RESTORE)
        {
            set_quick_mode(true, false);
        }

        const int mp_target = get_mp_max() / 2;

        if (get_mp() >= mp_target && you.restore_exertion != EXERT_NORMAL)
        {
            set_exertion(you.restore_exertion, false);
        }
    }
}

void inc_sp(int sp_gain, bool silent, bool manual)
{
    if (you.species == SP_DJINNI)
        return inc_hp(sp_gain);

    if (sp_gain < 1 || you.sp == you.sp_max)
        return;

    /*
    you.duration[DUR_EXHAUSTED] -= sp_gain;
    if (you.duration[DUR_EXHAUSTED] < 0)
        you.duration[DUR_EXHAUSTED] = 0;
        */

    you.sp += sp_gain;
    _restore_exertion_mode();

    if (you.sp > you.sp_max)
        you.sp = you.sp_max;

    you.redraw_stamina_points = true;

    if (_should_stop_resting(you.sp, you.sp_max))
        interrupt_activity(AI_FULL_SP);
}

void inc_mp(int mp_gain, bool silent)
{
    ASSERT(!crawl_state.game_is_arena());

    if (you.species == SP_DJINNI)
        return inc_hp(mp_gain);

    if (mp_gain < 1 || you.mp == you.mp_max)
        return;

    you.mp += mp_gain;
    _restore_exertion_mode();

    if (you.mp > you.mp_max)
        you.mp = you.mp_max;

    you.redraw_magic_points = true;

    if (_should_stop_resting(you.mp, you.mp_max))
        interrupt_activity(AI_FULL_MP);
}

// Note that "max_too" refers to the base potential, the actual
// resulting max value is subject to penalties, bonuses, and scalings.
// To avoid message spam, don't take notes when HP increases.
void inc_hp(int hp_gain)
{
    ASSERT(!crawl_state.game_is_arena());

    if (hp_gain < 1 || you.hp == you.hp_max)
        return;

    if (you.species == SP_DJINNI && you.hp > you.hp_max / 2 && (you.restore_exertion == EXERT_POWER || you.restore_exertion == EXERT_FOCUS))
    {
        set_exertion(you.restore_exertion, false);
    }

    you.hp += hp_gain;

    if (you.hp > you.hp_max)
        you.hp = you.hp_max;

    you.redraw_hit_points = true;

    if (_should_stop_resting(you.hp, you.hp_max))
        interrupt_activity(AI_FULL_HP);
}

void rot_hp(int hp_loss)
{
    if (!player_rotted() && hp_loss > 0)
        you.redraw_magic_points = true;

    const int initial_rot = you.hp_max_adj_temp;
    you.hp_max_adj_temp -= hp_loss;

    // don't allow too much rot
    int min_rot_allowed = 60;
    switch(crawl_state.difficulty)
    {
        case DIFFICULTY_EASY:
            min_rot_allowed = 80;
            break;
        case DIFFICULTY_STANDARD:
            min_rot_allowed = 60;
            break;
        case DIFFICULTY_CHALLENGE:
            min_rot_allowed = 40;
            break;
        case DIFFICULTY_NIGHTMARE:
            min_rot_allowed = 20;
            break;
        default:
            // should not be possible
            break;
    }

    you.hp_max_adj_temp = max(-(get_real_hp(false, true) - 1) * min_rot_allowed / 100,
                              you.hp_max_adj_temp);
    if (initial_rot == you.hp_max_adj_temp)
        return;

    calc_hp();

    if (you.species != SP_GHOUL)
        xom_is_stimulated(hp_loss * 25);

    you.redraw_hit_points = true;
}

int unrot_hp(int hp_recovered)
{
    int hp_balance = 0;
    if (hp_recovered > -you.hp_max_adj_temp)
    {
        hp_balance = hp_recovered + you.hp_max_adj_temp;
        you.hp_max_adj_temp = 0;
    }
    else
        you.hp_max_adj_temp += hp_recovered;
    calc_hp();

    you.redraw_hit_points = true;
    if (!player_rotted())
        you.redraw_magic_points = true;
    return hp_balance;
}

int player_rotted()
{
    return -you.hp_max_adj_temp || you.mp_frozen_summons;
}

void rot_mp(int mp_loss)
{
    you.mp_max_adj -= mp_loss;
    calc_mp();
    you.redraw_magic_points = true;
}

bool freeze_summons_mp(int mp_loss)
{
    if (get_mp_max() < mp_loss)
        return false;

    you.mp_frozen_summons += mp_loss;
    calc_mp();
    you.redraw_magic_points = true;
    return true;
}

void unfreeze_summons_mp(int amount)
{
    if (amount == -1)
        you.mp_frozen_summons = 0;
    else
        you.mp_frozen_summons = max(0, you.mp_frozen_summons - amount);

    calc_mp();
    you.redraw_magic_points = true;
}

void inc_max_hp(int hp_gain)
{
    you.hp += hp_gain;
    you.hp_max_adj_perm += hp_gain;
    calc_hp();

    take_note(Note(NOTE_MAXHP_CHANGE, you.hp_max));
    you.redraw_hit_points = true;
}

void dec_max_hp(int hp_loss)
{
    you.hp_max_adj_perm -= hp_loss;
    calc_hp();

    take_note(Note(NOTE_MAXHP_CHANGE, you.hp_max));
    you.redraw_hit_points = true;
}

// Use of floor: false = hp max, true = hp min. {dlb}
void deflate_hp(int new_level, bool floor)
{
    ASSERT(!crawl_state.game_is_arena());

    if (floor && you.hp < new_level)
        you.hp = new_level;
    else if (!floor && you.hp > new_level)
        you.hp = new_level;

    // Must remain outside conditional, given code usage. {dlb}
    you.redraw_hit_points = true;
}

void set_hp(int new_amount)
{
    ASSERT(!crawl_state.game_is_arena());

    you.hp = new_amount;

    if (you.hp > you.hp_max)
        you.hp = you.hp_max;

    // Must remain outside conditional, given code usage. {dlb}
    you.redraw_hit_points = true;
}

void set_sp(int new_amount)
{
    ASSERT(!crawl_state.game_is_arena());

    you.sp = new_amount;

    if (you.sp > you.sp_max)
        you.sp = you.sp_max;

    // Must remain outside conditional, given code usage. {dlb}
    you.redraw_stamina_points = true;
}

void set_mp(int new_amount)
{
    ASSERT(!crawl_state.game_is_arena());

    you.mp = new_amount;

    if (you.mp > you.mp_max)
        you.mp = you.mp_max;

    take_note(Note(NOTE_MP_CHANGE, you.mp, you.mp_max));

    // Must remain outside conditional, given code usage. {dlb}
    you.redraw_magic_points = true;
}

int effective_xl()
{
    const int effective = you.experience_level - player_mutation_level(MUT_INEXPERIENCED) * 2;
    return max(1, effective);
}

// If trans is true, being berserk and/or transformed is taken into account
// here. Else, the base hp is calculated. If rotted is true, calculate the
// real max hp you'd have if the rotting was cured.
int get_real_hp(bool trans, bool rotted, bool adjust_for_difficulty)
{
    int hitp;

    if (you.species == SP_MOON_TROLL)
        hitp  = 80;
    else
        hitp  = effective_xl() * 7 + 8;

    hitp += you.hp_max_adj_perm;
    /*
    // Important: we shouldn't add Heroism boosts here.
    hitp += effective_xl() * you.skill(SK_FIGHTING, 5, true) / 70
          + (you.skill(SK_FIGHTING, 3, true) + 1) / 2;
          */

    // Racial modifier.
    hitp *= 10 + species_hp_modifier(you.species);
    hitp /= 10;

    // Mutations that increase HP by a percentage
    hitp *= 100 + (player_mutation_level(MUT_ROBUST) * 10)
                + (you.attribute[ATTR_DIVINE_VIGOUR] * 5)
                + (player_mutation_level(MUT_RUGGED_BROWN_SCALES) ?
                   player_mutation_level(MUT_RUGGED_BROWN_SCALES) * 2 + 1 : 0)
                - (player_mutation_level(MUT_FRAIL) * 10);

    hitp /= 100;

    if (!rotted)
        hitp += you.hp_max_adj_temp;

    if (trans)
        hitp += you.scan_artefacts(ARTP_HP);

    // Being berserk makes you resistant to damage. I don't know why.
    if (trans && you.berserk())
        hitp = hitp * 3 / 2;

    if (trans) // Some transformations give you extra hp.
        hitp = hitp * form_hp_mod() / 10;

#if TAG_MAJOR_VERSION == 34
    if (trans && player_equip_unrand(UNRAND_ETERNAL_TORMENT))
        hitp = hitp * 4 / 5;
#endif

    if (adjust_for_difficulty)
    {
        hitp = player_pool_modifier(hitp);
    }

    hitp = max(10, hitp);

    return hitp;
}

int get_real_sp(bool include_items)
{
    int max_sp = 100;

    max_sp += player_mutation_level(MUT_HIGH_STAMINA) * 25;
    max_sp -= player_mutation_level(MUT_LOW_STAMINA) * 25;
    max_sp += you.wearing(EQ_RINGS, RING_STAMINA) * 25;
    max_sp += you.scan_artefacts(ARTP_STAMINA);

    max_sp = player_pool_modifier(max_sp);
    max_sp = max(max_sp, 25);

    return max_sp;
}

int get_real_mp(bool include_items, bool rotted)
{
    int max_mp = 100;

    // Analogous to ROBUST/FRAIL
    max_mp += + (player_mutation_level(MUT_HIGH_MAGIC) * 25)
              + (you.attribute[ATTR_DIVINE_VIGOUR] * 10)
              - (player_mutation_level(MUT_LOW_MAGIC) * 25)
              + species_mp_modifier(you.species) * 25
              ;

    // This is our "rotted" base, applied after multipliers
    max_mp += you.mp_max_adj;

    // Now applied after scaling so that power items are more useful -- bwr
    if (include_items)
    {
        const int num_magic_rings = you.wearing(EQ_RINGS, RING_MAGICAL_POWER);
        for (int i = 0; i < num_magic_rings; i++)
            max_mp += 25;
        max_mp += you.scan_artefacts(ARTP_MAGICAL_POWER);

        if (you.wearing(EQ_STAFF, STAFF_POWER))
            max_mp += 50;
    }

    if (include_items && you.wearing_ego(EQ_WEAPON, SPWPN_ANTIMAGIC))
        max_mp /= 3;

    max_mp = player_pool_modifier(max_mp);
    max_mp = max(max_mp, 25);

    if (!rotted)
        max_mp -= you.mp_frozen_summons;

    max_mp = max(max_mp, 0);

    return max_mp;
}

int get_unfrozen_mp()
{
    return you.mp + you.mp_frozen_summons;
}

bool player_regenerates_hp()
{
    if (player_mutation_level(MUT_SLOW_REGENERATION) == 3)
        return false;
    if (you.species == SP_VAMPIRE && you.hunger_state <= HS_STARVING)
        return false;
    return true;
}

bool player_regenerates_sp()
{
    return true;
}

bool player_regenerates_mp()
{
    // Don't let DD use magic shield for free HP, since their
    // damage shaving is enough. (due, dpeg)
    if (you.magic_shield() && you.species == SP_DEEP_DWARF)
        return false;
    // Pakellas blocks MP regeneration.
    if (have_passive(passive_t::no_mp_regen) || player_under_penance(GOD_PAKELLAS))
        return false;
    return true;
}

int get_contamination_level()
{
    const int glow = you.magic_contamination;

    if (glow > 60000)
        return glow / 20000 + 3;
    if (glow > 40000)
        return 5;
    if (glow > 25000)
        return 4;
    if (glow > 15000)
        return 3;
    if (glow > 5000)
        return 2;
    if (glow > 0)
        return 1;

    return 0;
}

/**
 * Provide a description of the given contamination 'level'.
 *
 * @param cont  A contamination 'tier', corresponding to a nonlinear
 *              contamination value; generated by get_contamination_level().
 *
 * @return      A string describing the player when in the given contamination
 *              level.
 */
string describe_contamination(int cont)
{
    /// Mappings from contamination levels to descriptions.
    static const string contam_descriptions[] =
    {
        "",
        "You are very lightly contaminated with residual magic.",
        "You are contaminated with residual magic.",
        "You are heavily infused with residual magic.",
        "You are practically glowing with residual magic!",
        "Your entire body has taken on an eerie glow!",
        "You are engulfed in a nimbus of crackling magics!",
    };

    ASSERT(cont >= 0);
    return contam_descriptions[min((size_t) cont,
                                   ARRAYSZ(contam_descriptions) - 1)];
}

// controlled is true if the player actively did something to cause
// contamination (such as drink a known potion of resistance),
// status_only is true only for the status output
void contaminate_player(int change, bool controlled, bool msg)
{
    ASSERT(!crawl_state.game_is_arena());

    int old_amount = you.magic_contamination;
    int old_level  = get_contamination_level();
    int new_level  = 0;

    if (change > 0 && player_equip_unrand(UNRAND_ETHERIC_CAGE))
        change *= 2;

    you.magic_contamination = max(0, min(250000,
                                         you.magic_contamination + change));

    new_level = get_contamination_level();

    if (you.magic_contamination != old_amount)
        dprf("change: %d  radiation: %d", change, you.magic_contamination);

    if (msg && new_level >= 1 && old_level <= 1 && new_level != old_level)
        mpr(describe_contamination(new_level));
    else if (msg && new_level != old_level)
    {
        if (old_level == 1 && new_level == 0)
            mpr("Your magical contamination has completely faded away.");
        else
        {
            mprf((change > 0) ? MSGCH_WARN : MSGCH_RECOVERY,
                 "You feel %s contaminated with magical energies.",
                 (change > 0) ? "more" : "less");
        }

        if (change > 0)
            xom_is_stimulated(new_level * 25);

        if (old_level > 1 && new_level <= 1 && you.invisible())
        {
            mpr("You fade completely from view now that you are no longer "
                "glowing from magical contamination.");
        }
    }

    if (you.magic_contamination > 0)
        learned_something_new(HINT_GLOWING);

    // Zin doesn't like mutations or mutagenic radiation.
    if (you_worship(GOD_ZIN))
    {
        // Whenever the glow status is first reached, give a warning message.
        if (old_level < 2 && new_level >= 2)
            did_god_conduct(DID_CAUSE_GLOWING, 0, false);
        // If the player actively did something to increase glowing,
        // Zin is displeased.
        else if (controlled && change > 0 && old_level > 1)
            did_god_conduct(DID_CAUSE_GLOWING, 1 + new_level, true);
    }
}

/**
 * Increase the player's confusion duration.
 *
 * @param amount   The number of turns to increase confusion duration by.
 * @param quiet    Whether to suppress messaging on success/failure.
 * @param force    Whether to ignore resistance (used only for intentional
 *                 self-confusion, e.g. via ambrosia).
 * @return         Whether confusion was successful.
 */
bool confuse_player(int amount, bool quiet, bool force)
{
    ASSERT(!crawl_state.game_is_arena());

    if (amount <= 0)
        return false;

    if (!force && you.clarity())
    {
        if (!quiet)
            mpr("You feel momentarily confused.");
        return false;
    }

    if (!force && you.duration[DUR_DIVINE_STAMINA] > 0)
    {
        if (!quiet)
            mpr("Your divine stamina protects you from confusion!");
        return false;
    }

    const int old_value = you.duration[DUR_CONF];
    you.increase_duration(DUR_CONF, amount, 40);

    if (you.duration[DUR_CONF] > old_value)
    {
        you.check_awaken(500);

        if (!quiet)
        {
            mprf(MSGCH_WARN, "You are %sconfused.",
                 old_value > 0 ? "more " : "");
        }

        learned_something_new(HINT_YOU_ENCHANTED);

        xom_is_stimulated((you.duration[DUR_CONF] - old_value)
                           / BASELINE_DELAY);
    }

    return true;
}

void paralyse_player(string source, int amount)
{
    if (!amount)
        amount = 2 + random2(6 + you.duration[DUR_PARALYSIS] / BASELINE_DELAY);

    you.paralyse(nullptr, amount, source);
}

bool poison_player(int amount, string source, string source_aux, bool force)
{
    ASSERT(!crawl_state.game_is_arena());

    if (you.duration[DUR_DIVINE_STAMINA] > 0)
    {
        mpr("Your divine stamina protects you from poison!");
        return false;
    }

    if (player_res_poison() >= 3)
    {
        dprf("Cannot poison, you are immune!");
        return false;
    }
    else if (!force && player_res_poison() > 0 && !one_chance_in(3))
        return false;

    const int old_value = you.duration[DUR_POISONING];
    const bool was_fatal = poison_is_lethal();

    if (player_res_poison() < 0)
        amount *= 2;

    you.duration[DUR_POISONING] += amount * 1000;

    if (you.duration[DUR_POISONING] > old_value)
    {
        if (poison_is_lethal() && !was_fatal)
            mprf(MSGCH_DANGER, "You are lethally poisoned!");
        else
        {
            mprf(MSGCH_WARN, "You are %spoisoned.",
                old_value > 0 ? "more " : "");
        }

        learned_something_new(HINT_YOU_POISON);
    }

    you.props["poisoner"] = source;
    you.props["poison_aux"] = source_aux;

    // Display the poisoned segment of our health, in case we take no damage
    you.redraw_hit_points = true;

    return amount;
}

int get_player_poisoning()
{
    if (player_res_poison() < 3)
    {
        // Approximate the effect of damage shaving by giving the first
        // 25 points of poison damage for 'free'
        if (you.species == SP_DEEP_DWARF)
            return max(0, (you.duration[DUR_POISONING] / 1000) - 25);
        else
            return you.duration[DUR_POISONING] / 1000;
    }
    else
        return 0;
}

// The amount of aut needed for poison to end if
// you.duration[DUR_POISONING] == dur, assuming no Chei/DD shenanigans.
// This function gives the following behavior:
// * 1/15 of current poison is removed every 10 aut normally
// * but speed of poison is capped between 0.025 and 1.000 HP/aut
static double _poison_dur_to_aut(double dur)
{
    // Poison already at minimum speed.
    if (dur < 15.0 * 250.0)
        return dur / 25.0;
    // Poison is not at maximum speed.
    if (dur < 15.0 * 10000.0)
        return 150.0 + 10.0 * log(dur / (15.0 * 250.0)) / log(15.0 / 14.0);
    return 150.0 + (dur - 15.0 * 10000.0) / 1000.0
                 + 10.0 * log(10000.0 / 250.0) / log(15.0 / 14.0);
}

// The inverse of the above function, i.e. the amount of poison needed
// to last for aut time.
static double _poison_aut_to_dur(double aut)
{
    // Amount of time that poison lasts at minimum speed.
    if (aut < 150.0)
        return aut * 25.0;
    // Amount of time that poison exactly at the maximum speed lasts.
    const double aut_from_max_speed = 150.0 + 10.0 * log(40.0) / log(15.0 / 14.0);
    if (aut < aut_from_max_speed)
        return 15.0 * 250.0 * exp(log(15.0 / 14.0) / 10.0 * (aut - 150.0));
    return 15.0 * 10000.0 + 1000.0 * (aut - aut_from_max_speed);
}

void handle_player_poison(int delay)
{
    const double cur_dur = you.duration[DUR_POISONING];
    const double cur_aut = _poison_dur_to_aut(cur_dur);

    // If Cheibriados has slowed your life processes, poison affects you less
    // quickly (you take the same total damage, but spread out over a longer
    // period of time).
    const double delay_scaling = (GOD_CHEIBRIADOS == you.religion && you.piety >= piety_breakpoint(0)) ? 2.0 / 3.0 : 1.0;

    const double new_aut = cur_aut - ((double) delay) * delay_scaling;
    const double new_dur = _poison_aut_to_dur(new_aut);

    const int decrease = you.duration[DUR_POISONING] - (int) new_dur;

    // Transforming into a form with no metabolism merely suspends the poison
    // but doesn't let your body get rid of it.
    // Hungry vampires are less affected by poison (not at all when bloodless).
    if (you.is_artificial() || you.undead_state()
        && (you.undead_state() != US_SEMI_UNDEAD
            || x_chance_in_y(4 - you.hunger_state, 4)))
    {
        return;
    }

    // Other sources of immunity (Zin, staff of Olgreb) let poison dissipate.
    bool do_dmg = (player_res_poison() >= 3 ? false : true);

    int dmg = (you.duration[DUR_POISONING] / 1000)
               - ((you.duration[DUR_POISONING] - decrease) / 1000);

    // Approximate old damage shaving by giving immunity to small amounts
    // of poison. Stronger poison will do the same damage as for non-DD
    // until it goes below the threshold, which is a bit weird, but
    // so is damage shaving.
    if (you.species == SP_DEEP_DWARF && you.duration[DUR_POISONING] - decrease < 25000)
    {
        dmg = (you.duration[DUR_POISONING] / 1000)
            - (25000 / 1000);
        if (dmg < 0)
            dmg = 0;
    }

    msg_channel_type channel = MSGCH_PLAIN;
    const char *adj = "";

    if (dmg > 6)
    {
        channel = MSGCH_DANGER;
        adj = "extremely ";
    }
    else if (dmg > 2)
    {
        channel = MSGCH_WARN;
        adj = "very ";
    }

    if (do_dmg && dmg > 0)
    {
        int oldhp = you.hp;
        ouch(dmg, KILLED_BY_POISON);
        if (you.hp < oldhp)
            mprf(channel, "You feel %ssick.", adj);
    }

    // Now decrease the poison in our system
    reduce_player_poison(decrease);
}

void reduce_player_poison(int amount)
{
    if (amount <= 0)
        return;

    you.duration[DUR_POISONING] -= amount;

    // Less than 1 point of damage remaining, so just end the poison
    if (you.duration[DUR_POISONING] < 1000)
        you.duration[DUR_POISONING] = 0;

    if (you.duration[DUR_POISONING] <= 0)
    {
        you.duration[DUR_POISONING] = 0;
        you.props.erase("poisoner");
        you.props.erase("poison_aux");
        mprf(MSGCH_RECOVERY, "You are no longer poisoned.");
    }

    you.redraw_hit_points = true;
}

// Takes *current* regeneration rate into account. Might sometimes be
// incorrect, but hopefully if so then the player is surviving with 1 HP.
bool poison_is_lethal()
{
    if (you.hp <= 0)
        return get_player_poisoning();
    if (get_player_poisoning() < you.hp)
        return false;
    return poison_survival() <= 0;
}

// Try to predict the minimum value of the player's health in the coming
// turns given the current poison amount and regen rate.
int poison_survival()
{
    if (!get_player_poisoning())
        return you.hp;
    const int rr = player_regen();
    const bool chei = have_passive(passive_t::slow_metabolism);
    const bool dd = (you.species == SP_DEEP_DWARF);
    const int amount = you.duration[DUR_POISONING];
    const double full_aut = _poison_dur_to_aut(amount);
    // Calculate the poison amount at which regen starts to beat poison.
    double min_poison_rate = 0.25;
    if (dd)
        min_poison_rate = 25.0/15.0;
    if (chei)
        min_poison_rate /= 1.5;
    int regen_beats_poison;
    if (rr <= (int) (100.0 * min_poison_rate))
        regen_beats_poison = dd ? 25000 : 0;
    else
    {
        regen_beats_poison = 150 * rr;
        if (chei)
            regen_beats_poison = 3 * regen_beats_poison / 2;
    }

    if (rr == 0)
        return min(you.hp, you.hp - amount / 1000 + regen_beats_poison / 1000);

    // Calculate the amount of time until regen starts to beat poison.
    double poison_duration = full_aut - _poison_dur_to_aut(regen_beats_poison);

    if (poison_duration < 0)
        poison_duration = 0;
    if (chei)
        poison_duration *= 1.5;

    // Worst case scenario is right before natural regen gives you a point of
    // HP, so consider the nearest two such points.
    const int predicted_regen = (int) ((((double) you.hit_points_regeneration) + rr * poison_duration / 10.0) / 100.0);
    double test_aut1 = (100.0 * predicted_regen - 1.0 - ((double) you.hit_points_regeneration)) / (rr / 10.0);
    double test_aut2 = (100.0 * predicted_regen + 99.0 - ((double) you.hit_points_regeneration)) / (rr / 10.0);

    if (chei)
    {
        test_aut1 /= 1.5;
        test_aut2 /= 1.5;
    }

    const int test_amount1 = _poison_aut_to_dur(full_aut - test_aut1);
    const int test_amount2 = _poison_aut_to_dur(full_aut - test_aut2);

    int prediction1 = you.hp;
    int prediction2 = you.hp;

    // Don't look backwards in time.
    if (test_aut1 > 0)
        prediction1 -= (amount / 1000 - test_amount1 / 1000 - (predicted_regen - 1));
    prediction2 -= (amount / 1000 - test_amount2 / 1000 - predicted_regen);

    return min(prediction1, prediction2);
}

bool miasma_player(actor *who, string source_aux)
{
    ASSERT(!crawl_state.game_is_arena());

    if (you.res_rotting() || you.duration[DUR_DEATHS_DOOR])
        return false;

    if (you.duration[DUR_DIVINE_STAMINA] > 0)
    {
        mpr("Your divine stamina protects you from the miasma!");
        return false;
    }

    bool success = poison_player(5 + roll_dice(3, 12),
                                 who ? who->name(DESC_A) : "",
                                 source_aux);

    if (coinflip())
    {
        you.rot(who, 1);
        success = true;
    }

    if (one_chance_in(3))
    {
        slow_player(10 + random2(5));
        success = true;
    }

    return success;
}

bool napalm_player(int amount, string source, string source_aux)
{
    ASSERT(!crawl_state.game_is_arena());

    if (player_res_sticky_flame() || amount <= 0 || you.duration[DUR_WATER_HOLD] || feat_is_watery(grd(you.pos())))
        return false;

    const int old_value = you.duration[DUR_LIQUID_FLAMES];
    you.increase_duration(DUR_LIQUID_FLAMES, amount, 100);

    if (you.duration[DUR_LIQUID_FLAMES] > old_value)
        mprf(MSGCH_WARN, "You are covered in liquid flames!");

    you.props["sticky_flame_source"] = source;
    you.props["sticky_flame_aux"] = source_aux;

    return true;
}

void dec_napalm_player(int delay)
{
    delay = min(delay, you.duration[DUR_LIQUID_FLAMES]);

    if (feat_is_watery(grd(you.pos())))
    {
        if (you.ground_level())
            mprf(MSGCH_WARN, "The flames go out!");
        else
            mprf(MSGCH_WARN, "You dip into the water, and the flames go out!");
        you.duration[DUR_LIQUID_FLAMES] = 0;
        you.props.erase("sticky_flame_source");
        you.props.erase("sticky_flame_aux");
        return;
    }

    mprf(MSGCH_WARN, "You are covered in liquid flames!");

    const int hurted = resist_adjust_damage(&you, BEAM_FIRE,
                                            random2avg(9, 2) + 1);

    you.expose_to_element(BEAM_STICKY_FLAME, 2);
    maybe_melt_player_enchantments(BEAM_STICKY_FLAME, hurted * delay / BASELINE_DELAY);

    ouch(hurted * delay / BASELINE_DELAY, KILLED_BY_BURNING);

    you.duration[DUR_LIQUID_FLAMES] -= delay;
    if (you.duration[DUR_LIQUID_FLAMES] <= 0)
    {
        you.props.erase("sticky_flame_source");
        you.props.erase("sticky_flame_aux");
    }
}

bool slow_player(int turns)
{
    ASSERT(!crawl_state.game_is_arena());

    if (turns <= 0)
        return false;

    if (check_stasis())
        return false;

    // Doubling these values because moving while slowed takes twice the
    // usual delay.
    turns = haste_mul(turns);
    int threshold = haste_mul(100);

    if (you.duration[DUR_SLOW] >= threshold * BASELINE_DELAY)
        mpr("You already are as slow as you could be.");
    else
    {
        if (you.duration[DUR_SLOW] == 0)
            mpr("You feel yourself slow down.");
        else
            mpr("You feel as though you will be slow longer.");

        you.increase_duration(DUR_SLOW, turns, threshold);
        learned_something_new(HINT_YOU_ENCHANTED);
    }

    return true;
}

void dec_slow_player(int delay)
{
    if (!you.duration[DUR_SLOW])
        return;

    if (you.duration    [DUR_SLOW] > BASELINE_DELAY)
    {
        // Make slowing and hasting effects last as long.
        you.duration[DUR_SLOW] -= you.duration[DUR_HASTE]
            ? haste_mul(delay) : delay;
    }

    if (you.torpor_slowed())
    {
        you.duration[DUR_SLOW] = 1;
        return;
    }
    if (you.props.exists(TORPOR_SLOWED_KEY))
        you.props.erase(TORPOR_SLOWED_KEY);

    if (you.duration[DUR_SLOW] <= BASELINE_DELAY)
    {
        mprf(MSGCH_DURATION, "You feel yourself speed up.");
        you.duration[DUR_SLOW] = 0;
    }
}

// Exhaustion should last as long as slowing.
int dec_exhaust_player(int amount)
{
    if (!you.duration[DUR_EXHAUSTED])
        return amount;

    if (you.duration[DUR_EXHAUSTED] > BASELINE_DELAY)
    {
        const int to_lose = min(amount, you.duration[DUR_EXHAUSTED]);
        you.duration[DUR_EXHAUSTED] -= you.duration[DUR_HASTE]
                                       ? haste_mul(to_lose) : to_lose;
        amount -= to_lose;
    }
    if (you.duration[DUR_EXHAUSTED] <= BASELINE_DELAY)
    {
        mprf(MSGCH_DURATION, "You feel less exhausted.");
        amount = max(0, -you.duration[DUR_EXHAUSTED]);
        you.duration[DUR_EXHAUSTED] = 0;
        you.redraw_evasion = true;
        you.redraw_armour_class = true;

        _restore_exertion_mode();
    }

    return amount;
}

bool haste_player(int turns, bool rageext, source_type source)
{
    ASSERT(!crawl_state.game_is_arena());

    if (turns <= 0)
        return false;

    if (check_stasis())
        return false;

    // Cutting the nominal turns in half since hasted actions take half the
    // usual delay.
    turns = haste_div(turns);
    const int threshold = 400;

    if (!you.duration[DUR_HASTE])
        mpr("You feel yourself speed up.");
    else if (you.duration[DUR_HASTE] > threshold * BASELINE_DELAY)
        mpr("You already have as much speed as you can handle.");
    else if (!rageext)
    {
        mpr("You feel as though your hastened speed will last longer.");
//        contaminate_player(1000, true); // always deliberate
    }

    you.increase_duration(DUR_HASTE, turns, threshold, nullptr);
    you.duration_source[DUR_HASTE] = source;

    return true;
}

void dec_haste_player(int delay)
{
    if (!you.duration[DUR_HASTE])
        return;

    if (you.duration[DUR_HASTE] > BASELINE_DELAY)
    {
        int old_dur = you.duration[DUR_HASTE];

        you.duration[DUR_HASTE] -= delay;

        int threshold = 6 * BASELINE_DELAY;
        // message if we cross the threshold
        if (old_dur > threshold && you.duration[DUR_HASTE] <= threshold)
        {
            mprf(MSGCH_DURATION, "Your extra speed is starting to run out.");
            if (coinflip())
                you.duration[DUR_HASTE] -= BASELINE_DELAY;
        }
    }
    else if (you.duration[DUR_HASTE] <= BASELINE_DELAY)
    {
        if (!you.duration[DUR_BERSERK])
            mprf(MSGCH_DURATION, "You feel yourself slow down.");
        you.duration[DUR_HASTE] = 0;
    }
}

void dec_disease_player(int delay)
{
    if (you.disease)
    {
        int rr = 50;

        // Extra regeneration means faster recovery from disease.
        // But not if not actually regenerating!
        if (player_regenerates_hp())
            rr += _player_bonus_regen();

        // Trog's Hand.
        if (you.duration[DUR_TROGS_HAND])
            rr += 100;

        rr = div_rand_round(rr * delay, 50);

        you.disease -= rr;
        if (you.disease < 0)
            you.disease = 0;

        if (you.disease == 0)
            mprf(MSGCH_RECOVERY, "You feel your health improve.");
    }
}

static void _dec_elixir_hp(int delay)
{
    you.duration[DUR_ELIXIR_HEALTH] -= delay;
    if (you.duration[DUR_ELIXIR_HEALTH] < 0)
        you.duration[DUR_ELIXIR_HEALTH] = 0;

    int heal = (delay * you.hp_max / 10) / BASELINE_DELAY;
    if (!you.duration[DUR_DEATHS_DOOR])
        inc_hp(heal);
}

static void _dec_elixir_mp(int delay)
{
    you.duration[DUR_ELIXIR_MAGIC] -= delay;
    if (you.duration[DUR_ELIXIR_MAGIC] < 0)
        you.duration[DUR_ELIXIR_MAGIC] = 0;

    int heal = (delay * you.mp_max / 10) / BASELINE_DELAY;
    inc_mp(heal * 3);
}

void dec_elixir_player(int delay)
{
    if (you.duration[DUR_ELIXIR_HEALTH])
        _dec_elixir_hp(delay);
    if (you.duration[DUR_ELIXIR_MAGIC])
        _dec_elixir_mp(delay);
}

void dec_ambrosia_player(int delay)
{
    if (!you.duration[DUR_AMBROSIA])
        return;

    // ambrosia ends when confusion does.
    if (!you.confused())
        you.duration[DUR_AMBROSIA] = 0;

    you.duration[DUR_AMBROSIA] = max(0, you.duration[DUR_AMBROSIA] - delay);

    // 3-5 per turn, 9-50 over (3-10) turns
    const int hp_restoration = div_rand_round(delay*(3 + random2(3)), BASELINE_DELAY);
    const int mp_restoration = div_rand_round(delay*(3 + random2(3)), BASELINE_DELAY);
    const int sp_restoration = div_rand_round(delay*(3 + random2(3)), BASELINE_DELAY);

    if (!you.duration[DUR_DEATHS_DOOR])
        inc_hp(you.scale_device_healing(hp_restoration));

    inc_mp(mp_restoration * 3);
    inc_sp(sp_restoration * 3);

    if (!you.duration[DUR_AMBROSIA])
        mpr("You feel less invigorated.");
}

bool invis_allowed(bool quiet, string *fail_reason)
{
    string msg;
    bool success = true;

    if (you.haloed() && you.halo_radius() != -1)
    {
        msg = "Your halo prevents invisibility.";
        success = false;
    }
    else if (you.backlit(false))
    {
        msg = "Invisibility will do you no good right now";
        if (!quiet && !yesno((msg + "; use anyway?").c_str(), false, 'n'))
        {
            canned_msg(MSG_OK);
            success = false;
            quiet = true; // since we just said something
        }
        msg += ".";
    }

    if (!success)
    {
        if (fail_reason)
            *fail_reason = msg;
        if (!quiet)
            mpr(msg);
    }
    return success;
}

bool flight_allowed(bool quiet, string *fail_reason)
{
    string msg;
    bool success = true;

    if (get_form()->forbids_flight())
    {
        msg = you.form == TRAN_TREE ? "Your roots keep you in place."
            : "You can't fly in this form.";
        success = false;
    }
    else if (you.liquefied_ground())
    {
        msg = "You can't fly while stuck in liquid ground.";
        success = false;
    }
    else if (you.duration[DUR_GRASPING_ROOTS])
    {
        msg = "The grasping roots prevent you from becoming airborne.";
        success = false;
    }

    if (!success)
    {
        if (fail_reason)
            *fail_reason = msg;
        if (!quiet)
            mpr(msg);
    }
    return success;
}

void float_player()
{
    if (you.fishtail)
    {
        mpr("Your tail turns into legs as you fly out of the water.");
        merfolk_stop_swimming();
    }
    else if (you.tengu_flight())
        mpr("You swoop lightly up into the air.");
    else
        mpr("You fly up into the air.");

    if (you.species == SP_TENGU)
        you.redraw_evasion = true;
}

void fly_player(int pow, bool already_flying)
{
    if (!flight_allowed())
        return;

    bool standing = !you.airborne() && !already_flying;
    if (!already_flying)
        mprf(MSGCH_DURATION, "You feel %s buoyant.", standing ? "very" : "more");

    you.increase_duration(DUR_FLIGHT, 25 + random2(pow), 100);

    if (standing)
        float_player();
}

static void _enable_emergency_flight()
{
    mpr("You can't land here! You focus on prolonging your flight, but the "
        "process is draining.");
    you.props[EMERGENCY_FLIGHT_KEY] = true;
}

/**
 * Handle the player's flight ending. Apply emergency flight if needed.
 *
 * @param quiet         Should we notify the player flight is ending?
 * @return              If flight was ended.
 */
bool land_player(bool quiet)
{
    // there was another source keeping you aloft
    if (you.airborne())
        return false;

    // Handle landing on (formerly) instakill terrain
    if (is_feat_dangerous(orig_terrain(you.pos()), true, false))
    {
        _enable_emergency_flight();
        return false;
    }

    if (!quiet)
        mpr("You float gracefully downwards.");
    if (you.species == SP_TENGU)
        you.redraw_evasion = true;

    you.attribute[ATTR_FLIGHT_UNCANCELLABLE] = 0;
    // Re-enter the terrain.
    move_player_to_grid(you.pos(), false);
    return true;
}

static void _end_water_hold()
{
    you.duration[DUR_WATER_HOLD] = 0;
    you.duration[DUR_WATER_HOLD_IMMUNITY] = 1;
    you.props.erase("water_holder");
}

bool player::clear_far_engulf()
{
    if (!you.duration[DUR_WATER_HOLD])
        return false;

    monster * const mons = monster_by_mid(you.props["water_holder"].get_int());
    if (!mons || !adjacent(mons->pos(), you.pos()))
    {
        if (you.res_water_drowning())
            mpr("The water engulfing you falls away.");
        else
            mpr("You gasp with relief as air once again reaches your lungs.");

        _end_water_hold();
        return true;
    }
    return false;
}

void handle_player_drowning(int delay)
{
    if (you.duration[DUR_WATER_HOLD] == 1)
    {
        if (!you.res_water_drowning())
            mpr("You gasp with relief as air once again reaches your lungs.");
        _end_water_hold();
    }
    else
    {
        monster* mons = monster_by_mid(you.props["water_holder"].get_int());
        if (!mons || !adjacent(mons->pos(), you.pos()))
        {
            if (you.res_water_drowning())
                mpr("The water engulfing you falls away.");
            else
                mpr("You gasp with relief as air once again reaches your lungs.");

            _end_water_hold();

        }
        else if (you.res_water_drowning())
        {
            // Reset so damage doesn't ramp up while able to breathe
            you.duration[DUR_WATER_HOLD] = 10;
        }
        else if (!you.res_water_drowning())
        {
            you.duration[DUR_WATER_HOLD] += delay;
            int dam =
                div_rand_round((28 + stepdown((float)you.duration[DUR_WATER_HOLD], 28.0))
                                * delay,
                                BASELINE_DELAY * 10);
            ouch(dam, KILLED_BY_WATER, mons->mid);
            mprf(MSGCH_WARN, "Your lungs strain for air!");
        }
    }
}

int count_worn_ego(int which_ego)
{
    int result = 0;
    for (int slot = EQ_MIN_ARMOUR; slot <= EQ_MAX_ARMOUR; ++slot)
    {
        if (you.equip[slot] != -1 && !you.melded[slot]
            && get_armour_ego_type(you.inv1[you.equip[slot]]) == which_ego)
        {
            result++;
        }
    }

    return result;
}

player::player()
{
    chr_god_name.clear();
    chr_species_name.clear();
    chr_class_name.clear();
    // Permanent data:
    your_name.clear();
    species          = SP_UNKNOWN;
    char_class       = JOB_UNKNOWN;
    type             = MONS_PLAYER;
    mid              = MID_PLAYER;
    position.reset();
    returnPosition.reset();

#ifdef WIZARD
    wizard = Options.wiz_mode == WIZ_YES;
    explore = Options.explore_mode == WIZ_YES;
#else
    wizard = false;
    explore = false;
#endif
    birth_time       = time(0);

    // Long-term state:
    elapsed_time     = 0;
    elapsed_time_at_last_input = 0;

    hp               = 0;
    hp_max           = 0;
    hp_max_adj_temp  = 0;
    hp_max_adj_perm  = 0;

    sp               = 0;
    sp_max           = 0;
    mp     = 0;
    mp_max = 0;
    mp_max_adj       = 0;
    mp_frozen_summons        = 0;

    stat_loss.init(0);
    base_stats.init(0);

    hunger          = HUNGER_DEFAULT;
    hunger_state    = HS_SATIATED;
    disease         = 0;
    max_level       = 1;
    hit_points_regeneration   = 0;
    magic_points_regeneration = 0;
    stamina_points_regeneration = 0;
    experience       = 0;
    total_experience = 0;
    experience_level = 1;
    gold             = 0;
    zigs_completed   = 0;
    zig_max          = 0;

    equip.init(-1);
    equip_slot_cursed_level.init(0);
    melded.reset();
    unrand_reacts.reset();

    symbol          = MONS_PLAYER;
    form            = TRAN_NONE;

    for (auto &item : inv1)
        item.clear();
    for (auto &item : inv2)
        item.clear();
    runes.reset();
    obtainable_runes = 20;

    spells.init(SPELL_NO_SPELL);
    old_vehumet_gifts.clear();
    spell_no        = 0;
    vehumet_gifts.clear();
    chapter  = CHAPTER_ORB_HUNTING;
    royal_jelly_dead = false;
    transform_uncancellable = false;
    fishtail = false;

    pet_target      = MHITNOT;

    duration.init(0);
    duration_source.init(SRC_UNDEFINED);
    apply_berserk_penalty = false;
    berserk_penalty = 0;
    attribute.init(0);
    // Default to flying the first time you wear boots of flying.
    attribute[ATTR_LAST_FLIGHT_STATUS] = 1;
    quiver.init(ENDOFPACK);

    last_timer_effect.init(0);
    next_timer_effect.init(20 * BASELINE_DELAY);

    dead = false;
    lives = 0;
    deaths = 0;

    temperature = 1; // 1 is min; 15 is max.
    temperature_last = 1;

    xray_vision = false;

    init_skills();

    skill_menu_do = SKM_NONE;
    skill_menu_view = SKM_NONE;

    transfer_from_skill = SK_NONE;
    transfer_to_skill = SK_NONE;
    transfer_skill_points = 0;
    transfer_total_skill_points = 0;

    skill_cost_level = 1;
    exp_available = 0;

    item_description.init(255);
    unique_items.init(UNIQ_NOT_EXISTS);
    unique_creatures.reset();
    force_autopickup.init(0);

    kills = KillMaster();

    where_are_you    = BRANCH_DUNGEON;
    depth            = 1;

    branch_stairs.init(0);

    religion         = GOD_NO_GOD;
    jiyva_second_name.clear();
    piety            = 0;
    piety_hysteresis = 0;
    gift_timeout     = 0;
    saved_good_god_piety = 0;
    previous_good_god = GOD_NO_GOD;
    penance.init(0);
    worshipped.init(0);
    num_current_gifts.init(0);
    num_total_gifts.init(0);
    one_time_ability_used.reset();
    piety_max.init(0);
    exp_docked.init(0);
    exp_docked_total.init(0);

    mutation.init(0);
    innate_mutation.init(0);
    temp_mutation.init(0);
    demonic_traits.clear();
    sacrifices.init(0);

    magic_contamination = 0;

    had_book.reset();
    seen_spell.reset();
    seen_weapon.init(0);
    seen_armour.init(0);
    seen_misc.reset();

    octopus_king_rings = 0;

    normal_vision    = LOS_RADIUS;
    current_vision   = LOS_RADIUS;

    real_time_ms     = chrono::milliseconds::zero();
    real_time_delta  = chrono::milliseconds::zero();
    num_turns        = 0;
    exploration      = 0;

    last_view_update = 0;

    spell_letter_table.init(-1);
    ability_letter_table.init(ABIL_NON_ABILITY);

    uniq_map_tags.clear();
    uniq_map_names.clear();
    vault_list.clear();

    global_info = PlaceInfo();
    global_info.assert_validity();

    m_quiver = player_quiver();

    props.clear();

    beholders.clear();
    fearmongers.clear();
    dactions.clear();
    level_stack.clear();
    type_ids.init(false);

    banished_by.clear();
    banished_power = 0;

    last_mid = 0;
    last_cast_spell = SPELL_NO_SPELL;

    // Non-saved UI state:
    prev_targ        = MHITNOT;
    prev_grd_targ.reset();
    prev_move.reset();

    travel_x         = 0;
    travel_y         = 0;
    travel_z         = level_id();

    running.clear();
    travel_ally_pace = false;
    received_weapon_warning = false;
    received_noskill_warning = false;
    wizmode_teleported_into_rock = false;
    ash_init_bondage(this);
    digging = false;

    delay_queue.clear();

    last_keypress_time = chrono::system_clock::now();

    action_count.clear();

    branches_left.reset();

    // Volatile (same-turn) state:
    turn_is_over     = false;
    banished         = false;

    wield_change         = false;
    redraw_quiver        = false;
    redraw_status_lights = false;
    redraw_hit_points    = false;
    redraw_stamina_points= false;
    redraw_magic_points  = false;
    redraw_temperature   = false;
    redraw_stats.init(false);
    redraw_experience    = false;
    redraw_armour_class  = false;
    redraw_evasion       = false;
    redraw_hit_chance    = false;
    redraw_title         = false;

    flash_colour        = BLACK;
    flash_where         = nullptr;

    time_taken          = 0;
    shield_blocks       = 0;

    abyss_speed         = 0;
    for (int i = 0; i < NUM_SEEDS; i++)
        game_seeds[i] = get_uint32();

    old_hunger          = hunger;
    transit_stair       = DNGN_UNSEEN;
    entering_level      = false;

    reset_escaped_death();
    on_current_level    = true;
    seen_portals        = 0;
    frame_no            = 0;

    amplification       = 1;
    exertion            = EXERT_NORMAL;
    restore_exertion    = EXERT_NORMAL;

    rune_charges.init(0);
    rune_curse_active.reset();
    branch_requires_runes.init(0);
    first_hit_time      = 0;

    max_exp             = 0;
    current_form_spell  = SPELL_NO_SPELL;
    current_form_spell_failure  = 0;
    last_to_hit_chance  = 0;
    last_be_hit_chance  = 0;
    last_hit_resistance = 0;
    last_damage         = 0;
    last_damage_resist  = 0;
    monsters_recently_seen = 0;
    summoned.init(MID_NOBODY);

    save                = nullptr;
    prev_save_version.clear();

    clear_constricted();
    constricting        = 0;
    stamina_flags       = 0;
    peace               = 1000;

    // Protected fields:
    for (branch_iterator it; it; ++it)
    {
        branch_info[it->id].branch = it->id;
        branch_info[it->id].assert_validity();
    }
}

void player::init_skills()
{
    auto_training = !(Options.default_manual_training);
    skills.init(0);
    train.init(false);
    train_alt.init(false);
    training.init(0);
    can_train.reset();
    skill_points.init(0);
    ct_skill_points.init(0);
    skill_order.init(MAX_SKILL_ORDER);
    exercises.clear();
    exercises_all.clear();
}

player_save_info& player_save_info::operator=(const player& rhs)
{
    name             = rhs.your_name;
    experience       = rhs.experience;
    experience_level = rhs.experience_level;
    wizard           = rhs.wizard;
    species          = rhs.species;
    species_name     = rhs.chr_species_name;
    class_name       = rhs.chr_class_name;
    religion         = rhs.religion;
    god_name         = rhs.chr_god_name;
    jiyva_second_name= rhs.jiyva_second_name;

    // [ds] Perhaps we should move game type to player?
    saved_game_type  = crawl_state.type;

    return *this;
}

bool player_save_info::operator<(const player_save_info& rhs) const
{
    return experience_level > rhs.experience_level
           || (experience_level == rhs.experience_level && name < rhs.name);
}

string player_save_info::short_desc() const
{
    ostringstream desc;

    const string qualifier = game_state::game_type_name_for(saved_game_type);
    if (!qualifier.empty())
        desc << "[" << qualifier << "] ";

    desc << name << ", a level " << experience_level << ' '
         << species_name << ' ' << class_name;

    if (religion == GOD_JIYVA)
        desc << " of " << god_name << " " << jiyva_second_name;
    else if (religion != GOD_NO_GOD)
        desc << " of " << god_name;

#ifdef WIZARD
    if (wizard)
        desc << " (WIZ)";
#endif

    return desc.str();
}

player::~player()
{
    if (CrawlIsCrashing && save)
    {
        save->abort();
        delete save;
        save = nullptr;
    }
    ASSERT(!save); // the save file should be closed or deleted
}

bool player::airborne() const
{
    // Might otherwise be airborne, but currently stuck to the ground
    if (you.duration[DUR_GRASPING_ROOTS] || get_form()->forbids_flight())
        return false;

    if (duration[DUR_FLIGHT]
        || you.species == SP_DJINNI
        || you.props[EMERGENCY_FLIGHT_KEY].get_bool()
        || attribute[ATTR_PERM_FLIGHT]
        || get_form()->enables_flight())
    {
        return true;
    }

    return false;
}

bool player::is_banished() const
{
    return banished;
}

bool player::is_sufficiently_rested() const
{
    // Only return false if resting will actually help.
    const bool hp_is_good = hp >= _rest_trigger_level(hp_max) || !player_regenerates_hp();
    const bool mp_is_good = mp >= _rest_trigger_level(mp_max) || !player_regenerates_mp();
    const bool sp_is_good = sp >= _rest_trigger_level(sp_max) || !player_regenerates_sp();
    return hp_is_good && mp_is_good && sp_is_good;
}

bool player::in_water() const
{
    return ground_level() && !you.can_water_walk() && feat_is_water(grd(pos()));
}

bool player::in_lava() const
{
    return ground_level() && feat_is_lava(grd(pos()));
}

bool player::in_liquid() const
{
    return in_water() || in_lava() || liquefied_ground();
}

bool player::can_swim(bool permanently) const
{
    // Transforming could be fatal if it would cause unequipment of
    // stat-boosting boots or heavy armour.
    return (species_can_swim(species)
            || body_size(PSIZE_BODY) >= SIZE_GIANT
            || !permanently)
                && form_can_swim();
}

/// Can the player do a passing imitation of a notorious Palestinian?
bool player::can_water_walk() const
{
    return have_passive(passive_t::water_walk)
           || you.props.exists(TEMP_WATERWALK_KEY);
}

int player::visible_igrd(const coord_def &where) const
{
    if (grd(where) == DNGN_LAVA
        || (grd(where) == DNGN_DEEP_WATER
            && !species_likes_water(species)))
    {
        return NON_ITEM;
    }

    return igrd(where);
}

bool player::has_spell(spell_type spell) const
{
    return find(begin(spells), end(spells), spell) != end(spells);
}

bool player::cannot_speak() const
{
    if (silenced(pos()))
        return true;

    if (cannot_move()) // we allow talking during sleep ;)
        return true;

    // No transform that prevents the player from speaking yet.
    // ... yet setting this would prevent saccing junk and similar activities
    // for no good reason.
    return false;
}

static const string shout_verbs[] = {"shout", "yell", "scream"};
static const string felid_shout_verbs[] = {"meow", "yowl", "caterwaul"};

/**
 * What verb should be used to describe the player's shouting?
 *
 * @param directed      Whether you're shouting at anyone in particular.
 * @return              A shouty kind of verb.
 */
string player::shout_verb(bool directed) const
{
    if (!get_form()->shout_verb.empty())
        return get_form()->shout_verb;

    const int screaminess = max(player_mutation_level(MUT_SCREAM) - 1, 0);

    if (species != SP_FELID)
        return shout_verbs[screaminess];
    if (directed && screaminess == 0)
        return "hiss"; // hiss at, not meow at
    return felid_shout_verbs[screaminess];
}

/**
 * How loud are the player's shouts?
 *
 * @return The noise produced by a single player shout.
 */
int player::shout_volume() const
{
    const int base_noise = 12 + get_form()->shout_volume_modifier;

    if (player_mutation_level(MUT_SCREAM))
        return base_noise + 2 * (player_mutation_level(MUT_SCREAM) - 1);

    return base_noise;
}

void player::god_conduct(conduct_type thing_done, int level)
{
    ::did_god_conduct(thing_done, level);
}

void player::banish(actor* /*agent*/, const string &who, const int power,
                    bool force)
{
    ASSERT(!crawl_state.game_is_arena());
    if (brdepth[BRANCH_ABYSS] == -1)
        return;

    if (elapsed_time <= attribute[ATTR_BANISHMENT_IMMUNITY])
    {
        mpr("You resist the pull of the Abyss.");
        return;
    }

    if (!force && player_in_branch(BRANCH_ABYSS)
        && x_chance_in_y(you.depth, brdepth[BRANCH_ABYSS]))
    {
        mpr("You wobble for a moment.");
        return;
    }

    banished    = true;
    banished_by = who;
    banished_power = power;
}

// For semi-undead species (Vampire!) reduce food cost for spells and abilities
// to 50% (hungry, very hungry) or zero (near starving, starving).
int calc_hunger(int food_cost)
{
    if (you.undead_state() == US_SEMI_UNDEAD && you.hunger_state < HS_SATIATED)
    {
        if (you.hunger_state <= HS_NEAR_STARVING)
            return 0;

        return food_cost/2;
    }
    return food_cost;
}

bool player::paralysed() const
{
    return duration[DUR_PARALYSIS];
}

bool player::cannot_move() const
{
    return paralysed() || petrified();
}

bool player::confused() const
{
    return duration[DUR_CONF];
}

bool player::caught() const
{
    return attribute[ATTR_HELD];
}

bool player::petrifying() const
{
    return duration[DUR_PETRIFYING];
}

bool player::petrified() const
{
    return duration[DUR_PETRIFIED];
}

bool player::liquefied_ground() const
{
    return liquefied(pos())
           && ground_level() && !is_insubstantial();
}

int player::shield_block_penalty() const
{
    return 5 * shield_blocks * shield_blocks;
}

/**
 * Returns whether the player currently has any kind of shield.
 *
 * XXX: why does this function exist?
 */
bool player::shielded() const
{
    return shield()
           || duration[DUR_MAGIC_SHIELD]
           || duration[DUR_DIVINE_SHIELD]
           || player_mutation_level(MUT_LARGE_BONE_PLATES) > 0
           || qazlal_sh_boost() > 0
           || attribute[ATTR_BONE_ARMOUR] > 0
           || you.wearing(EQ_AMULET_PLUS, AMU_REFLECTION) > 0
           || you.scan_artefacts(ARTP_SHIELDING);
}

int player::shield_bonus() const
{
    const int shield_class = player_shield_class();
    if (shield_class <= 0)
        return -100;

    return random2avg(shield_class * 2, 2) / 3 - 1;
}

int player::shield_bypass_ability(int tohit) const
{
    return 15 + tohit / 2;
}

void player::shield_block_succeeded(actor *foe)
{
    actor::shield_block_succeeded(foe);

    shield_blocks++;
    practise(EX_SHIELD_BLOCK);
    if (shield())
        count_action(CACT_BLOCK, shield()->sub_type);
    else
        count_action(CACT_BLOCK, -1, BLOCK_OTHER); // non-shield block
}

int player::missile_deflection() const
{
    if (attribute[ATTR_DEFLECT_MISSILES])
        return 2;

    if (attribute[ATTR_REPEL_MISSILES]
        || player_mutation_level(MUT_DISTORTION_FIELD) == 3
        || scan_artefacts(ARTP_RMSL, true)
        || have_passive(passive_t::upgraded_storm_shield))
    {
        return 1;
    }

    return 0;
}

void player::ablate_deflection()
{
    const int orig_defl = missile_deflection();

    bool did_something = false;
    if (attribute[ATTR_DEFLECT_MISSILES])
    {
        const int power = calc_spell_power(SPELL_DEFLECT_MISSILES, true, false);
        if (one_chance_in(2 + power / 8))
        {
            attribute[ATTR_DEFLECT_MISSILES] = 0;
            did_something = true;
        }
    }
    else if (attribute[ATTR_REPEL_MISSILES])
    {
        const int power = calc_spell_power(SPELL_REPEL_MISSILES, true, false);
        if (one_chance_in(2 + power / 8))
        {
            attribute[ATTR_REPEL_MISSILES] = 0;
            did_something = true;
        }
    }

    if (did_something)
    {
        // We might also have the effect from a non-expiring source.
        mprf(MSGCH_DURATION, "You feel %s from missiles.",
                             missile_deflection() < orig_defl
                                 ? "less protected"
                                 : "your spell is no longer protecting you");
    }
}

/**
 * What's the base value of the penalties the player receives from their
 * body armour?
 *
 * Used as the base for adjusted armour penalty calculations, as well as for
 * stealth penalty calculations.
 *
 * @return  The player's body armour's PARM_EVASION, if any.
 */
int player::unadjusted_body_armour_penalty() const
{
    const item_def *body_armour = slot_item(EQ_BODY_ARMOUR, false);
    if (!body_armour)
        return 0;

    const int ev_reduction = -property(*body_armour, PARM_EVASION);
    return ev_reduction / 10;
}

/**
 * The encumbrance penalty to the player for their worn body armour.
 *
 * @param scale     A scale to multiply the result by, to avoid precision loss.
 * @return          A penalty to EV based quadratically on body armour
 *                  encumbrance.
 */
int player::adjusted_body_armour_penalty(int scale) const
{
    const int base_ev_penalty =
        max(0, unadjusted_body_armour_penalty()
                   - you.mutation[MUT_STURDY_FRAME] * 2);

    // New formula for effect of str on aevp: (2/5) * evp^2 / (str+3)
    const int ev_part = 2 * base_ev_penalty * base_ev_penalty;
    const int armour_skill = skill(SK_ARMOUR, 10);
    const int str = strength();
    const int armour_reduction = (450 - armour_skill) * scale / 450;
    const int strength_reduction = 5 * (str + 3);
    const int result = ev_part * armour_reduction / strength_reduction;
    return max(0, result);
}

/**
 * The encumbrance penalty to the player for their worn shield.
 *
 * @param scale     A scale to multiply the result by, to avoid precision loss.
 * @return          A penalty to EV based on shield weight.
 */
int player::adjusted_shield_penalty(int scale) const
{
    const item_def *shield_l = slot_item(EQ_SHIELD, false);
    if (!shield_l)
        return 0;

    const int base_shield_penalty = -property(*shield_l, PARM_EVASION);
    return max(0, ((base_shield_penalty * scale) - skill(SK_SHIELDS, scale)
                  / player_shield_racial_factor() * 10) / 10);
}

float player::get_shield_skill_to_offset_penalty(const item_def &item)
{
    int evp = property(item, PARM_EVASION);
    return -1 * evp * player_shield_racial_factor() / 10.0;
}

int player::armour_tohit_penalty(bool random_factor, int scale) const
{
    return maybe_roll_dice(1, adjusted_body_armour_penalty(scale), random_factor);
}

int player::shield_tohit_penalty(bool random_factor, int scale) const
{
    return maybe_roll_dice(1, adjusted_shield_penalty(scale), random_factor);
}

int player::skill(skill_type sk, int scale, bool real, bool drained) const
{
    // If you add another enhancement/reduction, be sure to change
    // SkillMenuSwitch::get_help() to reflect that

    // wizard racechange, or upgraded old save
    if (is_useless_skill(sk))
        return 0;

    // skills[sk] might not be updated yet if this is in the middle of
    // skill training, so make sure to use the correct value.
    // This duplicates code in check_skill_level_change(), unfortunately.
    int actual_skill = skills[sk];
    unsigned int effective_points = skill_points[sk];
    if (!real)
    {
        for (skill_type cross : get_crosstrain_skills(sk))
            effective_points += skill_points[cross] * 2 / 5;
    }
    effective_points = min(effective_points, skill_exp_needed(get_max_skill_level(), sk));
    while (1)
    {
        if (actual_skill < get_max_skill_level()
            && effective_points >= skill_exp_needed(actual_skill + 1, sk))
        {
            ++actual_skill;
        }
        else if (effective_points < skill_exp_needed(actual_skill, sk))
        {
            actual_skill--;
            ASSERT(actual_skill >= 0);
        }
        else
            break;
    }

    int level = actual_skill * scale
      + get_skill_progress(sk, actual_skill, effective_points, scale);
    if (real)
        return level;
    if (drained && you.attribute[ATTR_XP_DRAIN])
    {
        int drain_scale = max(0, (30 * 100 - you.attribute[ATTR_XP_DRAIN]) * scale);
        level = skill(sk, drain_scale, real, false);
        return max(0, (level - 30 * scale * you.attribute[ATTR_XP_DRAIN]) / (30 * 100));
    }

    if (penance[GOD_ASHENZARI])
        level = max(level - 4 * scale, level / 2);
    else if (have_passive(passive_t::bondage_skill_boost))
    {
        if (skill_boost.count(sk)
            && skill_boost.find(sk)->second)
        {
            level = ash_skill_boost(sk, scale);
        }
    }
    if ((sk == SK_LONG_BLADES || sk == SK_SHORT_BLADES)
        && player_equip_unrand(UNRAND_FENCERS))
    {
        level = min(level + 4 * scale, get_max_skill_level() * scale);
    }
    if (duration[DUR_HEROISM] && sk <= SK_LAST_MUNDANE)
        level = min(level + 5 * scale, get_max_skill_level() * scale);
    return level;
}

int player_icemail_armour_class()
{
    if (!you.mutation[MUT_ICEMAIL])
        return 0;

    return you.duration[DUR_ICEMAIL_DEPLETED] ? 0 : ICEMAIL_MAX;
}

/**
 * How many points of AC/SH does the player get from their current bone armour?
 *
 * ((power / 100) + 0.5) * (# of corpses). (That is, between 0.5 and 1.5 AC+SH
 * per corpse.)
 * @return          The AC/SH bonus * 100. (For scale reasons.)
 */
static int _bone_armour_bonus()
{
    if (!you.attribute[ATTR_BONE_ARMOUR])
        return 0;

    const int power = calc_spell_power(SPELL_CIGOTUVIS_EMBRACE, true, false);
    // rounding errors here, but not sure of a good way to avoid that.
    return you.attribute[ATTR_BONE_ARMOUR] * (50 + power);
}

/**
 * How many points of AC does the player get from their sanguine armour, if
 * they have any?
 *
 * @return      The AC bonus * 100. (For scaling.)
 */
int sanguine_armour_bonus()
{
    if (!you.duration[DUR_SANGUINE_ARMOUR])
        return 0;

    const int mut_lev = you.mutation[MUT_SANGUINE_ARMOUR];
    // like iridescent, but somewhat moreso (when active)
    return 300 + mut_lev * 300;
}

/**
 * How much AC does the player get from an unenchanted version of the given
 * armour?
 *
 * @param armour    The armour in question.
 * @param scale     A value to multiply the result by. (Used to avoid integer
 *                  rounding.)
 * @return          The AC from that armour, including armour skill, mutations
 *                  & divine blessings, but not enchantments or egos.
 */
int player::base_ac_from(const item_def &armour, int scale) const
{
    const int base_ac     = property(armour, PARM_AC) * scale;
    const int beogh_bonus  = _player_armour_beogh_bonus(armour);

    // [ds] effectively: ac_value * (22 + Arm) / 22, where Arm =
    // Armour Skill + beogh_bonus / 2.
    const int AC
        = base_ac * (440 + skill(SK_ARMOUR, 20) + beogh_bonus * 10) / 440;

    // The deformed don't fit into body armour very well.
    // (This includes nagas and centaurs.)
    int deformity = player_mutation_level(MUT_DEFORMED);
    if (get_armour_slot(armour) == EQ_BODY_ARMOUR
        && (deformity || player_mutation_level(MUT_PSEUDOPODS)))
    {
        return AC - (base_ac * deformity) / 4;
    }

    return max(0, AC);
}

/**
 * What bonus AC are you getting from your species?
 *
 * Does not account for any real mutations, such as scales or thick skin, that
 * you may have as a result of your species.
 * @param temp Whether to account for transformations.
 * @returns how much AC you are getting from your species "fake mutations" * 100
 */
int player::racial_ac(bool temp) const
{
    int ac = 0;

    // drac scales suppressed in all serious forms, except dragon
    if (species_is_draconian(species)
        && (!player_is_shapechanged() || form == TRAN_DRAGON || !temp))
    {
        ac += 400 + 100 * (experience_level / 3);  // max 13
        if (species == SP_GREY_DRACONIAN) // no breath
            ac += 500;
    }

    if (!(player_is_shapechanged() && temp))
    {
        if (species == SP_NAGA)
            ac += 100 * experience_level / 3;  	// max 9 or so
        else if (species == SP_LAVA_ORC && temperature_effect(LORC_STONESKIN))
            ac += 300 + 100 * experience_level / 6;	// max 8 or so
        else if (species == SP_GARGOYLE)
        {
            ac += 200 + 100 * experience_level * 2 / 5	// max 20 or so
                       + 100 * (max(0, experience_level - 7) * 2 / 5);
        }
    }

    ac += player_mutation_level(MUT_EXOSKELETON) * 300;

    return ac;
}

int player::armour_class(bool /*calc_unid*/) const
{
    int AC = 0;

    for (int eq = EQ_MIN_ARMOUR; eq <= EQ_MAX_ARMOUR; ++eq)
    {
        if (eq == EQ_SHIELD)
            continue;

        if (!slot_item(static_cast<equipment_type>(eq)))
            continue;

        const item_def& item = inv1[equip[eq]];
        AC += base_ac_from(item, 100);
        AC += item.plus * 100;
    }

    AC += wearing(EQ_RINGS_PLUS, RING_PROTECTION) * 100;

    if (wearing_ego(EQ_WEAPON, SPWPN_PROTECTION))
        AC += 500;

    if (wearing_ego(EQ_SHIELD, SPARM_PROTECTION))
        AC += 300;

    AC += scan_artefacts(ARTP_AC) * 100;

    if (duration[DUR_ICY_ARMOUR])
        AC += 500 + you.props[ICY_ARMOUR_KEY].get_int() * 8;

    if (duration[DUR_MAGIC_ARMOUR])
        AC += 200 + you.props[MAGIC_ARMOUR_KEY].get_int() * 5;

    if (mutation[MUT_ICEMAIL])
        AC += 10 * player_icemail_armour_class();

    if (duration[DUR_QAZLAL_AC])
        AC += 300;

    if (duration[DUR_CORROSION])
        AC -= 400 * you.props["corrosion_amount"].get_int();

    AC += _bone_armour_bonus();
    AC += sanguine_armour_bonus();

    AC += get_form()->get_ac_bonus();

    AC += racial_ac(true);

    // Scale mutations, etc. Statues don't get an AC benefit from scales,
    // since the scales are made of the same stone as everything else.
    AC += player_mutation_level(MUT_TOUGH_SKIN)
          ? player_mutation_level(MUT_TOUGH_SKIN) * 100 : 0;                   // +1, +2, +3
    AC += player_mutation_level(MUT_SHAGGY_FUR)
          ? player_mutation_level(MUT_SHAGGY_FUR) * 100 : 0;                   // +1, +2, +3
    AC += player_mutation_level(MUT_GELATINOUS_BODY)
          ? (player_mutation_level(MUT_GELATINOUS_BODY) == 3 ? 200 : 100) : 0; // +1, +1, +2
    AC += _mut_level(MUT_IRIDESCENT_SCALES, MUTACT_FULL)
          ? 200 + _mut_level(MUT_IRIDESCENT_SCALES, MUTACT_FULL) * 200 : 0;    // +4, +6, +8
    AC += _mut_level(MUT_LARGE_BONE_PLATES, MUTACT_FULL)
          ? 100 + _mut_level(MUT_LARGE_BONE_PLATES, MUTACT_FULL) * 100 : 0;    // +2, +3, +4
    AC += _mut_level(MUT_ROUGH_BLACK_SCALES, MUTACT_FULL)
          ? 100 + _mut_level(MUT_ROUGH_BLACK_SCALES, MUTACT_FULL) * 300 : 0;   // +4, +7, +10
    AC += _mut_level(MUT_RUGGED_BROWN_SCALES, MUTACT_FULL) * 100;              // +1, +2, +3
    AC += _mut_level(MUT_ICY_BLUE_SCALES, MUTACT_FULL) * 100 +
          (_mut_level(MUT_ICY_BLUE_SCALES, MUTACT_FULL) > 1 ? 100 : 0);        // +1, +3, +4
    AC += _mut_level(MUT_MOLTEN_SCALES, MUTACT_FULL) * 100 +
          (_mut_level(MUT_MOLTEN_SCALES, MUTACT_FULL) > 1 ? 100 : 0);          // +1, +3, +4
    AC += _mut_level(MUT_SLIMY_GREEN_SCALES, MUTACT_FULL)
          ? 100 + _mut_level(MUT_SLIMY_GREEN_SCALES, MUTACT_FULL) * 100 : 0;   // +2, +3, +4
    AC += _mut_level(MUT_THIN_METALLIC_SCALES, MUTACT_FULL)
          ? 100 + _mut_level(MUT_THIN_METALLIC_SCALES, MUTACT_FULL) * 100 : 0; // +2, +3, +4
    AC += _mut_level(MUT_YELLOW_SCALES, MUTACT_FULL)
          ? 100 + _mut_level(MUT_YELLOW_SCALES, MUTACT_FULL) * 100 : 0;        // +2, +3, +4
    AC -= player_mutation_level(MUT_PHYSICAL_VULNERABILITY)
          ? player_mutation_level(MUT_PHYSICAL_VULNERABILITY) * 300 : 0;       // +3, +6, +9
    AC = player_ac_modifier(AC);
    AC = AC / 100;

    return AC;
}
 /**
  * Guaranteed damage reduction.
  *
  * The percentage of the damage received that is guaranteed to be reduced
  * by the armour. As the AC roll is done before GDR is applied, GDR is only
  * useful when the AC roll is inferior to it. Therefore a higher GDR means
  * more damage reduced, but also more often.
  *
  * \f[ GDR = 14 \times (base\_AC - 2)^\frac{1}{2} \f]
  *
  * \return GDR as a percentage.
  **/
int player::gdr_perc() const
{
    switch (form)
    {
    case TRAN_DRAGON:
        return 34; // base AC 8
    case TRAN_STATUE:
        return 39; // like plate (AC 10)
    case TRAN_TREE:
        return 48;
    default:
        break;
    }

    const item_def *body_armour = slot_item(EQ_BODY_ARMOUR, false);

    int body_base_AC = (species == SP_GARGOYLE ? 5 : 0);
    if (body_armour)
        body_base_AC += property(*body_armour, PARM_AC);

    // We take a sqrt here because damage prevented by GDR is
    // actually proportional to the square of the GDR percentage
    // (assuming you have enough AC).
    int gdr = 14 * sqrt(max(body_base_AC - 2, 0));

    return gdr;
}

/**
 * What is the player's actual, current EV, possibly relative to an attacker,
 * including various temporary penalties?
 *
 * @param evit     Penalty types which should be excluded from the calculation.
 * @param act      The creature that the player is attempting to evade, if any.
 *                 May be null.
 * @return         The player's relevant EV.
 */
int player::evasion(ev_ignore_type evit, const actor* act) const
{
    const int base_evasion = _player_evasion(evit);

    const int constrict_penalty = is_constricted() ? 3 : 0;

    const bool attacker_invis = act && !act->visible_to(this);
    const int invis_penalty = attacker_invis && !(evit & EV_IGNORE_HELPLESS) ?
                              10 : 0;

    int amount_of_stairs_penalty = 3;

    const int stairs_penalty = player_stair_delay()
                                && !(evit & EV_IGNORE_HELPLESS) ?
                                    amount_of_stairs_penalty :
                                    0;

    int ev = base_evasion - constrict_penalty - invis_penalty - stairs_penalty;

    ev = player_ev_modifier(ev);
    return ev;
}

bool player::heal(int amount, bool silent)
{
    if (amount > 0 && !silent)
        mprf("(hp+%d)", amount);

    ::inc_hp(amount);
    return true; /* TODO Check whether the player was healed. */
}

/**
 * What is the player's (current) mon_holy_type category?
 * Stays up to date with god for evil/unholy
 * Nonliving (statues, etc), undead, or alive.
 *
 * @param temp      Whether to consider temporary effects: forms,
 *                  petrification...
 * @return          The player's holiness category.
 */
mon_holy_type player::holiness(bool temp) const
{
    mon_holy_type holi = MH_NATURAL;
    if (species == SP_GARGOYLE ||
        temp && (form == TRAN_STATUE || form == TRAN_WISP || petrified()))
    {
        holi = MH_NONLIVING;
    }

    if (undead_state(temp))
        holi = MH_UNDEAD;

    if (species == SP_DEMONSPAWN)
        holi |= MH_UNHOLY;

    if (is_good_god(religion))
        holi |= MH_HOLY;

    if (is_evil_god(religion))
        holi |= MH_EVIL;

    // possible XXX: Monsters get evil/unholy bits set on spell selection
    //  should players?
    return holi;
}

bool player::undead_or_demonic() const
{
    // This is only for TSO-related stuff, so demonspawn are included.
    return undead_state() || species == SP_DEMONSPAWN;
}

bool player::holy_wrath_susceptible() const
{
    return undead_or_demonic();
}

bool player::is_holy(bool check_spells) const
{
    return bool(holiness() & MH_HOLY);
}

bool player::is_unholy(bool check_spells) const
{
    return bool(holiness() & (MH_DEMONIC | MH_UNHOLY));
}

bool player::is_evil(bool check_spells) const
{
    return bool(holiness() & (MH_UNDEAD | MH_EVIL));
}

// This is a stub. Check is used only for silver damage. Worship of chaotic
// gods should probably be checked in the non-existing player::is_unclean,
// which could be used for something Zin-related (such as a priestly monster).
int player::how_chaotic(bool /*check_spells_god*/) const
{
    return 0;
}

/**
 * Is the player currently 'artificial' (nonliving holiness)?
 *
 * @param temp  Whether to consider temporary effects (forms, status effects)
 * @return      Whether the player should be considered an artificial lifeform.
 */
bool player::is_artificial(bool temp) const
{
    return species == SP_GARGOYLE
           || temp && (form == TRAN_STATUE || petrified());
}

/**
 * Does the player need to breathe?
 *
 * Pretty much only matters for mephitic clouds, & confusing spores, & curare.
 *
 * @return  Whether the player has no need to breathe.
 */
bool player::is_unbreathing() const
{
    return !get_form()->breathes || petrified()
        || player_mutation_level(MUT_UNBREATHING);
}

bool player::is_insubstantial() const
{
    return form == TRAN_WISP;
}

int player::res_acid(bool calc_unid) const
{
    return player_res_acid(calc_unid);
}

int player::res_fire() const
{
    return player_res_fire();
}

int player::res_steam() const
{
    return player_res_steam();
}

int player::res_cold() const
{
    return player_res_cold();
}

int player::res_elec() const
{
    return player_res_electricity();
}

int player::res_water_drowning() const
{
    int rw = 0;

    if (is_unbreathing()
        || species == SP_MERFOLK && !form_changed_physiology()
        || species == SP_OCTOPODE && !form_changed_physiology()
        || form == TRAN_ICE_BEAST
        || form == TRAN_HYDRA)
    {
        rw++;
    }

    // A fiery lich/hot statue suffers from quenching but not drowning, so
    // neutral resistance sounds ok.
    if (species == SP_DJINNI)
        rw--;

    return rw;
}

int player::res_poison(bool temp) const
{
    return player_res_poison(true, temp);
}

int player::res_rotting(bool temp) const
{
    if (player_mutation_level(MUT_ROT_IMMUNITY)
        || temp && (is_artificial() || get_form()->res_rot()))
    {
        return 3;
    }

    switch (undead_state(temp))
    {
    default:
    case US_ALIVE:
        return 0;

    case US_HUNGRY_DEAD:
        return 1; // rottable by Zin, not by necromancy

    case US_SEMI_UNDEAD:
        if (temp && hunger_state < HS_SATIATED)
            return 1;
        return 0; // no permanent resistance

    case US_UNDEAD:
        return 3; // full immunity
    }
}

bool player::res_sticky_flame() const
{
    return player_res_sticky_flame();
}

int player::res_holy_energy(const actor *attacker) const
{
    if (undead_or_demonic())
        return -2;

    if (is_evil())
        return -1;

    if (is_holy())
        return 1;

    return 0;
}

int player::res_negative_energy(bool intrinsic_only) const
{
    return player_prot_life(!intrinsic_only, true, !intrinsic_only);
}

bool player::res_torment() const
{
    return player_res_torment();
}

bool player::res_wind() const
{
    // Full control of the winds around you can negate a hostile tornado.
    return duration[DUR_TORNADO] ? 1 : 0;
}

bool player::res_petrify(bool temp) const
{
    return player_mutation_level(MUT_PETRIFICATION_RESISTANCE)
           || temp && get_form()->res_petrify();
}

int player::res_constrict() const
{
    if (is_insubstantial())
        return 3;
    if (form == TRAN_PORCUPINE
        || player_mutation_level(MUT_SPINY))
    {
        return 3;
    }
    return 0;
}

int player::res_magic(bool /*calc_unid*/) const
{
    return player_res_magic();
}

int player_res_magic(bool calc_unid, bool temp)
{

    if (temp && you.form == TRAN_SHADOW)
        return MAG_IMMUNE;

    int rm = effective_xl() * species_mr_modifier(you.species);

    // randarts
    rm += MR_PIP * you.scan_artefacts(ARTP_MAGIC_RESISTANCE, calc_unid);

    // body armour
    const item_def *body_armour = you.slot_item(EQ_BODY_ARMOUR);
    if (body_armour)
        rm += armour_type_prop(body_armour->sub_type, ARMF_RES_MAGIC) * MR_PIP;

    // ego armours
    rm += MR_PIP * you.wearing_ego(EQ_ALL_ARMOUR, SPARM_MAGIC_RESISTANCE,
                                   calc_unid);

    // rings of magic resistance
    rm += MR_PIP * you.wearing(EQ_RINGS, RING_PROTECTION_FROM_MAGIC, calc_unid);

    // Mutations
    rm += MR_PIP * player_mutation_level(MUT_MAGIC_RESISTANCE);
    rm -= MR_PIP * player_mutation_level(MUT_MAGICAL_VULNERABILITY);

    // transformations
    if (you.form == TRAN_LICH && temp)
        rm += MR_PIP;

    // Trog's Hand
    if (you.duration[DUR_TROGS_HAND] && temp)
        rm += MR_PIP * 2;

    // Enchantment effect
    if (you.duration[DUR_LOWERED_MR] && temp)
        rm /= 2;

    if (rm < 0)
        rm = 0;

    rm = player_mr_modifier(rm);

    return rm;
}

/**
 * Is the player prevented from teleporting? If so, why?
 *
 * @param calc_unid     Whether to identify unknown items that prevent tele
 *                      (probably obsolete)
 * @param blinking      Are you blinking or teleporting?
 * @return              Why the player is prevented from teleporting, if they
 *                      are; else, the empty string.
 */
string player::no_tele_reason(bool calc_unid, bool blinking) const
{
    if (crawl_state.game_is_sprint() && !blinking)
        return "Long-range teleportation is disallowed in Dungeon Sprint.";

    if (species == SP_FORMICID)
        return pluralise(species_name(species)) + " cannot teleport.";

    vector<string> problems;

    if (duration[DUR_DIMENSION_ANCHOR])
        problems.emplace_back("locked down by Dimension Anchor");

    if (form == TRAN_TREE)
        problems.emplace_back("held in place by your roots");

    vector<item_def> notele_items;
    if (has_notele_item(calc_unid, &notele_items) || stasis())
    {
        vector<string> worn_notele;
        bool found_nonartefact = false;

        for (const auto &item : notele_items)
        {
            if (item.base_type == OBJ_WEAPONS)
            {
                problems.push_back(make_stringf("wielding %s",
                                                item.name(DESC_A).c_str()));
            }
            else
                worn_notele.push_back(item.name(DESC_A));
        }

        if (worn_notele.size() > (problems.empty() ? 3 : 1))
        {
            problems.push_back(
                make_stringf("wearing %s %s preventing teleportation",
                             number_in_words(worn_notele.size()).c_str(),
                             found_nonartefact ? "items": "artefacts"));
        }
        else if (!worn_notele.empty())
        {
            problems.push_back(
                make_stringf("wearing %s",
                             comma_separated_line(worn_notele.begin(),
                                                  worn_notele.end()).c_str()));
        }

        if (stasis())
        {
            // Formicids are handled above, other sources
            // of stasis will display this message:
            problems.emplace_back("affected by a buggy stasis");
        }
    }

    if (problems.empty())
        return ""; // no problem

    return make_stringf("You cannot teleport because you are %s.",
                        comma_separated_line(problems.begin(),
                                             problems.end()).c_str());
}

/**
 * Is the player prevented from teleporting/blinking right now? If so,
 * print why.
 *
 * @param calc_unid     Whether to identify unknown items that prevent tele
 *                      (probably obsolete)
 * @param blinking      Are you blinking or teleporting?
 * @return              Whether the player is prevented from teleportation.
 */
bool player::no_tele_print_reason(bool calc_unid, bool blinking) const
{
    const string reason = no_tele_reason(calc_unid, blinking);
    if (reason.empty())
        return false;

    mpr(reason);
    return true;
}

/**
 * Is the player prevented from teleporting/blinking right now?
 *
 * @param calc_unid     Whether to identify unknown items that prevent tele
 *                      (probably obsolete)
 * @param permit_id     Unused for players.
 * @param blinking      Are you blinking or teleporting?
 * @return              Whether the player is prevented from teleportation.
 */
bool player::no_tele(bool calc_unid, bool /*permit_id*/, bool blinking) const
{
    return no_tele_reason(calc_unid, blinking) != "";
}

bool player::fights_well_unarmed(int heavy_armour_penalty)
{
    return x_chance_in_y(skill(SK_UNARMED_COMBAT, 10), 200)
        && x_chance_in_y(2, 1 + heavy_armour_penalty);
}

bool player::cancellable_flight() const
{
    return duration[DUR_FLIGHT] && !permanent_flight()
           && !attribute[ATTR_FLIGHT_UNCANCELLABLE];
}

bool player::permanent_flight() const
{
    return attribute[ATTR_PERM_FLIGHT]
        || species == SP_DJINNI
        ;
}

bool player::racial_permanent_flight() const
{
    return player_mutation_level(MUT_TENGU_FLIGHT) >= 2
        || species == SP_DJINNI
        || player_mutation_level(MUT_BIG_WINGS);
}

bool player::tengu_flight() const
{
    // Only Tengu get perks for flying.
    return species == SP_TENGU && airborne();
}

/**
 * Returns the HP cost (per MP) of casting a spell.
 *
 * Checks to see if the player is wielding the Majin-Bo.
 *
 * @return        The HP cost (per MP) of casting a spell.
 **/
int player::spell_hp_cost() const
{
    int cost = 0;

    if (player_equip_unrand(UNRAND_MAJIN))
        cost += 1;

    return cost;
}

/**
 * Returns true if player spellcasting is considered unholy.
 *
 * Checks to see if the player is wielding the Majin-Bo.
 *
 * @return          Whether player spellcasting is an unholy act.
 */
bool player::spellcasting_unholy() const
{
    return player_equip_unrand(UNRAND_MAJIN);
}

/**
 * What is the player's (current) place on the Undead Spectrum?
 * (alive, hungry undead (ghoul), semi-undead (vampire), or very dead (mummy,
 * lich)
 *
 * @param temp  Whether to consider temporary effects (lichform)
 * @return      The player's undead state.
 */
undead_state_type player::undead_state(bool temp) const
{
    undead_state_type result = species_undead_type(you.species);
    if (temp)
    {
        switch(you.form)
        {
            case TRAN_NONE:
            case TRAN_APPENDAGE:
            case TRAN_BLADE_HANDS:
                break;
            case TRAN_LICH:
                result = US_UNDEAD;
                break;
            default:
                result = US_ALIVE;
        }
    }

    return result;
}

bool player::nightvision() const
{
    return have_passive(passive_t::nightvision);
}

reach_type player::reach_range() const
{
    const item_def *wpn = weapon();
    if (wpn)
        return weapon_reach(*wpn);
    return REACH_NONE;
}

monster_type player::mons_species(bool zombie_base) const
{
    return player_species_to_mons_species(species);
}

bool player::poison(actor *agent, int amount, bool force)
{
    return ::poison_player(amount, agent? agent->name(DESC_A, true) : "", "",
                           force);
}

void player::expose_to_element(beam_type element, int _strength,
                               bool slow_cold_blood)
{
    ::expose_player_to_element(element, _strength, slow_cold_blood);
}

void player::blink()
{
    cast_blink();
}

void player::teleport(bool now, bool wizard_tele)
{
    ASSERT(!crawl_state.game_is_arena());

    if (now)
        you_teleport_now(wizard_tele);
    else
        you_teleport();
}

int player::hurt(const actor *agent, int amount, beam_type flavour,
                 kill_method_type kill_type, string source, string aux,
                 bool /*cleanup_dead*/, bool /*attacker_effects*/, bool skip_details)
{
    // We ignore cleanup_dead here.
    if (!agent)
    {
        // FIXME: This can happen if a deferred_damage_fineff does damage
        // to a player from a dead monster. We should probably not do that,
        // but it could be tricky to fix, so for now let's at least avoid
        // a crash even if it does mean funny death messages.
        ouch(amount, kill_type, MID_NOBODY, aux.c_str(), false, source.c_str(), false, skip_details);
    }
    else
    {
        ouch(amount, kill_type, agent->mid, aux.c_str(),
             agent->visible_to(this), source.c_str(), false, skip_details);
    }

    if ((flavour == BEAM_DEVASTATION || flavour == BEAM_DISINTEGRATION)
        && can_bleed())
    {
        blood_spray(pos(), type, amount / 5);
    }

    return amount;
}

void player::drain_stat(stat_type s, int amount)
{
    lose_stat(s, amount);
}

bool player::rot(actor *who, int amount, bool quiet, bool /*no_cleanup*/)
{
    ASSERT(!crawl_state.game_is_arena());

    if (amount <= 0)
        return false;

    if (res_rotting() || duration[DUR_DEATHS_DOOR])
    {
        mpr("You feel terrible.");
        return false;
    }

    if (duration[DUR_DIVINE_STAMINA] > 0)
    {
        mpr("Your divine stamina protects you from decay!");
        return false;
    }

    rot_hp(amount);

    if (!quiet)
        mprf(MSGCH_WARN, "You feel your flesh rotting away!");

    learned_something_new(HINT_YOU_ROTTING);

    if (one_chance_in(4))
        sicken(50 + random2(100));

    return true;
}

void player::corrode_equipment(const char* corrosion_source, int degree)
{
    // rCorr protects against 50% of corrosion.
    if (res_corr())
    {
        degree = binomial(degree, 50);
        if (!degree)
        {
            dprf("rCorr protects.");
            return;
        }
    }
    // always increase duration, but...
    increase_duration(DUR_CORROSION, 10 + roll_dice(2, 4), 50,
                      make_stringf("%s corrodes you!",
                                   corrosion_source).c_str());

    // the more corrosion you already have, the lower the odds of more
    int prev_corr = props["corrosion_amount"].get_int();
    bool did_corrode = false;
    for (int i = 0; i < degree; i++)
        if (!x_chance_in_y(prev_corr, prev_corr + 9))
        {
            props["corrosion_amount"].get_int()++;
            prev_corr++;
            did_corrode = true;
        }

    if (did_corrode)
    {
        redraw_armour_class = true;
        wield_change = true;
    }
    return;
}

/**
 * Attempts to apply corrosion to the player and deals acid damage.
 *
 * @param evildoer the cause of this acid splash.
 * @param acid_strength The strength of the acid.
 * @param allow_corrosion Whether to try and apply the corrosion debuff.
 * @param hurt_msg A message to display when dealing damage.
 */
void player::splash_with_acid(const actor* evildoer, int acid_strength,
                              bool allow_corrosion, const char* hurt_msg)
{
    if (allow_corrosion && binomial(3, acid_strength + 1, 30))
        corrode_equipment();

    const int dam = roll_dice(4, acid_strength);
    const int post_res_dam = resist_adjust_damage(&you, BEAM_ACID, dam);

    if (post_res_dam > 0)
    {
        mpr(hurt_msg ? hurt_msg : "The acid burns!");

        if (post_res_dam < dam)
            canned_msg(MSG_YOU_RESIST);

        ouch(post_res_dam, KILLED_BY_ACID,
             evildoer ? evildoer->mid : MID_NOBODY);
    }
}

bool player::drain_exp(actor *who, bool quiet, int pow)
{
    return drain_player(pow, !quiet);
}

void player::confuse(actor *who, int str)
{
    confuse_player(str);
}

/**
 * Paralyse the player for str turns.
 *
 *  Duration is capped at 13.
 *
 * @param who Pointer to the actor who paralysed the player.
 * @param str The number of turns the paralysis will last.
 * @param source Description of the source of the paralysis.
 */
void player::paralyse(actor *who, int str, string source)
{
    ASSERT(!crawl_state.game_is_arena());

    // The shock is too mild to do damage.
    if (check_stasis())
        return;

    // The who check has an effect in a few cases, most notably making
    // Death's Door + Borg's paralysis unblockable.
    if (who && (duration[DUR_PARALYSIS] || duration[DUR_PARALYSIS_IMMUNITY]))
    {
        mpr("You shrug off the repeated paralysis!");
        return;
    }

    int &paralysis(duration[DUR_PARALYSIS]);

    if (source.empty() && who)
        source = who->name(DESC_A);

    if (!paralysis && !source.empty())
    {
        take_note(Note(NOTE_PARALYSIS, str, 0, source));
        props["paralysed_by"] = source;
    }

    mprf("You %s the ability to move!",
         paralysis ? "still don't have" : "suddenly lose");

    str *= BASELINE_DELAY;
    if (str > paralysis && (paralysis < 3 || one_chance_in(paralysis)))
        paralysis = str;

    if (paralysis > 13 * BASELINE_DELAY)
        paralysis = 13 * BASELINE_DELAY;

    stop_constricting_all();
    end_searing_ray();
}

void player::petrify(actor *who, bool force)
{
    ASSERT(!crawl_state.game_is_arena());

    if (res_petrify() && !force)
    {
        canned_msg(MSG_YOU_UNAFFECTED);
        return;
    }

    if (duration[DUR_DIVINE_STAMINA] > 0)
    {
        mpr("Your divine stamina protects you from petrification!");
        return;
    }

    if (petrifying())
    {
        mpr("Your limbs have turned to stone.");
        duration[DUR_PETRIFYING] = 1;
        return;
    }

    if (petrified())
        return;

    duration[DUR_PETRIFYING] = 3 * BASELINE_DELAY;

    redraw_evasion = true;
    mprf(MSGCH_WARN, "You are slowing down.");
}

bool player::fully_petrify(actor *foe, bool quiet)
{
    duration[DUR_PETRIFIED] = 6 * BASELINE_DELAY
                        + random2(4 * BASELINE_DELAY);
    redraw_evasion = true;
    mpr("You have turned to stone.");

    end_searing_ray();

    return true;
}

void player::slow_down(actor *foe, int str)
{
    ::slow_player(str);
}


int player::has_claws(bool allow_tran) const
{
    if (allow_tran)
    {
        // these transformations bring claws with them
        if (form == TRAN_DRAGON)
            return 3;

        // blade hands override claws
        if (form == TRAN_BLADE_HANDS)
            return 0;
    }

    return player_mutation_level(MUT_CLAWS, allow_tran);
}

bool player::has_usable_claws(bool allow_tran) const
{
    return !slot_item(EQ_GLOVES) && has_claws(allow_tran);
}

int player::has_talons(bool allow_tran) const
{
    // XXX: Do merfolk in water belong under allow_tran?
    if (fishtail)
        return 0;

    return player_mutation_level(MUT_TALONS, allow_tran);
}

bool player::has_usable_talons(bool allow_tran) const
{
    return !slot_item(EQ_BOOTS) && has_talons(allow_tran);
}

int player::has_hooves(bool allow_tran) const
{
    // XXX: Do merfolk in water belong under allow_tran?
    if (fishtail)
        return 0;

    return player_mutation_level(MUT_HOOVES, allow_tran);
}

bool player::has_usable_hooves(bool allow_tran) const
{
    return has_hooves(allow_tran)
           && (!slot_item(EQ_BOOTS)
               || wearing(EQ_BOOTS, ARM_CENTAUR_BARDING, true));
}

int player::has_fangs(bool allow_tran) const
{
    if (allow_tran)
    {
        // these transformations bring fangs with them
        if (form == TRAN_DRAGON)
            return 3;
    }

    return player_mutation_level(MUT_FANGS, allow_tran);
}

int player::has_usable_fangs(bool allow_tran) const
{
    return has_fangs(allow_tran);
}

int player::has_tail(bool allow_tran) const
{
    if (allow_tran)
    {
        // these transformations bring a tail with them
        if (form == TRAN_DRAGON)
            return 1;

        // Most transformations suppress a tail.
        if (!form_keeps_mutations())
            return 0;
    }

    // XXX: Do merfolk in water belong under allow_tran?
    if (species_is_draconian(species)
        || fishtail
        || player_mutation_level(MUT_STINGER, allow_tran))
    {
        return 1;
    }

    return 0;
}

int player::has_usable_tail(bool allow_tran) const
{
    // TSO worshippers don't use their stinger in order
    // to avoid poisoning.
    if (religion == GOD_SHINING_ONE
        && player_mutation_level(MUT_STINGER, allow_tran) > 0)
    {
        return 0;
    }

    return has_tail(allow_tran);
}

// Whether the player has a usable offhand for the
// purpose of punching.
bool player::has_usable_offhand() const
{
    if (player_mutation_level(MUT_MISSING_HAND))
        return false;
    if (shield())
        return false;

    const item_def* wp = slot_item(EQ_WEAPON);
    return !wp || hands_reqd(*wp) != HANDS_TWO;
}

bool player::has_usable_tentacle() const
{
    return usable_tentacles();
}

int player::usable_tentacles() const
{
    int numtentacle = has_usable_tentacles();

    if (numtentacle == 0)
        return false;

    int free_tentacles = numtentacle - num_constricting();

    if (shield())
        free_tentacles -= 2;

    const item_def* wp = slot_item(EQ_WEAPON);
    if (wp)
    {
        hands_reqd_type hands_req = hands_reqd(*wp);
        free_tentacles -= 2 * hands_req + 2;
    }

    return free_tentacles;
}

int player::has_pseudopods(bool allow_tran) const
{
    return player_mutation_level(MUT_PSEUDOPODS, allow_tran);
}

int player::has_usable_pseudopods(bool allow_tran) const
{
    return has_pseudopods(allow_tran);
}

int player::has_tentacles(bool allow_tran) const
{
    if (allow_tran)
    {
        // Most transformations suppress tentacles.
        if (!form_keeps_mutations())
            return 0;
    }

    if (species == SP_OCTOPODE && player_mutation_level(MUT_MISSING_HAND))
        return 7;
    else if (species == SP_OCTOPODE)
        return 8;

    return 0;
}

int player::has_usable_tentacles(bool allow_tran) const
{
    return has_tentacles(allow_tran);
}

bool player::sicken(int amount)
{
    ASSERT(!crawl_state.game_is_arena());

    if (res_rotting() || amount <= 0)
        return false;

    if (duration[DUR_DIVINE_STAMINA] > 0)
    {
        mpr("Your divine stamina protects you from disease!");
        return false;
    }

    mpr("You feel ill.");

    disease += amount * BASELINE_DELAY;
    if (disease > 210 * BASELINE_DELAY)
        disease = 210 * BASELINE_DELAY;

    return true;
}

/// Can the player see invisible things?
bool player::can_see_invisible(bool calc_unid) const
{
    if (crawl_state.game_is_arena())
        return true;

    if (wearing(EQ_RINGS, RING_SEE_INVISIBLE, calc_unid)
        // armour: (checks head armour only)
        || wearing_ego(EQ_HELMET, SPARM_SEE_INVISIBLE)
        // randart gear
        || scan_artefacts(ARTP_SEE_INVISIBLE, calc_unid) > 0)
    {
        return true;
    }

    return innate_sinv();
}

/// Can the player see invisible things without needing items' help?
bool player::innate_sinv() const
{
    // Possible to have both with a temp mutation.
    if (player_mutation_level(MUT_ACUTE_VISION)
        && !player_mutation_level(MUT_BLURRY_VISION))
    {
        return true;
    }

    // antennae give sInvis at 3
    if (player_mutation_level(MUT_ANTENNAE) == 3)
        return true;

    if (player_mutation_level(MUT_EYEBALLS) == 3)
        return true;

    if (have_passive(passive_t::sinv))
        return true;

    return false;
}

bool player::invisible() const
{
    return (duration[DUR_INVIS] || form == TRAN_SHADOW)
           && !backlit();
}

bool player::visible_to(const actor *looker) const
{
    if (crawl_state.game_is_arena())
        return false;

    const bool invis_to = invisible() && !looker->can_see_invisible()
                          && !in_water();
    if (this == looker)
        return !invis_to;

    const monster* mon = looker->as_monster();
    return mon->friendly()
        || (!mon->has_ench(ENCH_BLIND) && !invis_to);
}

/**
 * Is the player backlit?
 *
 * @param self_halo If true, ignore the player's self-halo.
 * @returns True if the player is backlit.
*/
bool player::backlit(bool self_halo) const
{
    return get_contamination_level() > 1
        || duration[DUR_CORONA]
        || duration[DUR_LIQUID_FLAMES]
        || duration[DUR_QUAD_DAMAGE]
        || !umbraed() && haloed() && (self_halo || halo_radius() == -1)
		|| player_mutation_level(MUT_GLOW) > 2;
}

bool player::umbra() const
{
    return !backlit() && umbraed() && !haloed();
}

// This is the imperative version.
void player::backlight()
{
    if (!duration[DUR_INVIS] && form != TRAN_SHADOW)
    {
        if (duration[DUR_CORONA])
            mpr("You glow brighter.");
        else
            mpr("You are outlined in light.");
        increase_duration(DUR_CORONA, random_range(15, 35), 250);
    }
    else
    {
        mpr("You feel strangely conspicuous.");
        increase_duration(DUR_CORONA, random_range(3, 5), 250);
    }
}

bool player::can_mutate() const
{
    return true;
}

/**
 * Can the player be mutated without rotting instead?
 *
 * @param temp      Whether to consider temporary modifiers (lichform)
 * @return Whether the player will mutate when mutated, instead of rotting.
 */
bool player::can_safely_mutate(bool temp) const
{
    if (!can_mutate())
        return false;

    return (undead_state(temp) == US_ALIVE || undead_state(temp) == US_SEMI_UNDEAD)
        && (you.species != SP_VAMPIRE || you.hunger_state > HS_SATIATED)
        ;
}

// Is the player too undead to bleed, rage, or polymorph?
bool player::is_lifeless_undead(bool temp) const
{
    if (undead_state() == US_SEMI_UNDEAD)
        return temp ? hunger_state <= HS_SATIATED : false;
    else
        return undead_state() != US_ALIVE;
}

bool player::can_polymorph() const
{
    return !(transform_uncancellable || is_lifeless_undead());
}

bool player::can_bleed(bool allow_tran) const
{
    // XXX: Lich and statue forms are still caught by the holiness checks below.
    if (allow_tran && !form_can_bleed(form))
        return false;

    if (is_lifeless_undead()
        || species == SP_DJINNI
        || holiness() & MH_NONLIVING)
    {   // demonspawn and demigods have a mere drop of taint
        return false;
    }

    return true;
}

bool player::is_stationary() const
{
    return form == TRAN_TREE;
}

bool player::malmutate(const string &reason)
{
    ASSERT(!crawl_state.game_is_arena());

    if (!can_mutate())
        return false;

    const mutation_type mut_quality = one_chance_in(5) ? RANDOM_MUTATION
                                                       : RANDOM_BAD_MUTATION;
    if (mutate(mut_quality, reason))
    {
        learned_something_new(HINT_YOU_MUTATED);
        return true;
    }
    return false;
}

bool player::polymorph(int pow)
{
    ASSERT(!crawl_state.game_is_arena());

    if (!can_polymorph())
        return false;

    transformation_type f = TRAN_NONE;

    // Be unreliable over lava. This is not that important as usually when
    // it matters you'll have temp flight and thus that pig will fly (and
    // when flight times out, we'll have roasted bacon).
    for (int tries = 0; tries < 3; tries++)
    {
        // Whole-body transformations only; mere appendage doesn't seem fitting.
        f = random_choose_weighted(
            100, TRAN_BAT,
            100, TRAN_FUNGUS,
            100, TRAN_PIG,
            100, TRAN_TREE,
            100, TRAN_PORCUPINE,
            100, TRAN_WISP,
             20, TRAN_SPIDER,
             20, TRAN_ICE_BEAST,
              5, TRAN_STATUE,
              2, TRAN_HYDRA,
              1, TRAN_DRAGON,
              0);
        // need to do a dry run first, as Zin's protection has a random factor
        if (transform(pow, f, true, true))
            break;
        f = TRAN_NONE;
    }

    if (f && transform(pow, f))
    {
        transform_uncancellable = true;
        return true;
    }
    return false;
}

bool player::is_icy() const
{
    return form == TRAN_ICE_BEAST;
}

bool player::is_fiery() const
{
    return false;
}

bool player::is_skeletal() const
{
    return false;
}

void player::shiftto(const coord_def &c)
{
    crawl_view.shift_player_to(c);
    set_position(c);
    clear_far_constrictions();
}

void player::reset_prev_move()
{
    prev_move.reset();
}

bool player::asleep() const
{
    return duration[DUR_SLEEP];
}

bool player::cannot_act() const
{
    return asleep() || cannot_move();
}

bool player::can_throw_large_rocks() const
{
    return species_can_throw_large_rocks(species) || you.strength(true) > 30;
}

bool player::can_smell() const
{
    return species != SP_MUMMY;
}

/**
 * Attempts to put the player to sleep.
 *
 * @param power     The power of the effect putting the player to sleep.
 * @param hibernate Whether the player is being put to sleep by 'ensorcelled
 *                  hibernation' (doesn't affect characters with rC, ignores
 *                  power), or by a normal sleep effect.
 */
void player::put_to_sleep(actor*, int power, bool hibernate)
{
    ASSERT(!crawl_state.game_is_arena());

    const bool valid_target = hibernate ? can_hibernate() : can_sleep();
    if (!valid_target)
    {
        canned_msg(MSG_YOU_UNAFFECTED);
        return;
    }

    if (duration[DUR_SLEEP_IMMUNITY])
    {
        mpr("You can't fall asleep again this soon!");
        return;
    }

    mpr("You fall asleep.");

    stop_constricting_all();
    end_searing_ray();
    stop_delay();
    flash_view(UA_MONSTER, DARKGREY);

    // As above, do this after redraw.
    const int dur = hibernate ? 3 + random2avg(5, 2) :
                                5 + random2avg(power/10, 5);
    set_duration(DUR_SLEEP, dur);
}

void player::awake()
{
    ASSERT(!crawl_state.game_is_arena());

    duration[DUR_SLEEP] = 0;
    duration[DUR_SLEEP_IMMUNITY] = 1;
    mpr("You wake up.");
    flash_view(UA_MONSTER, BLACK);
}

void player::check_awaken(int disturbance)
{
    if (asleep() && x_chance_in_y(disturbance + 1, 50))
        awake();
}

int player::beam_resists(bolt &beam, int hurted, bool doEffects, string source)
{
    return check_your_resists(hurted, beam.flavour, source, &beam, doEffects);
}

void player::set_place_info(PlaceInfo place_info)
{
    place_info.assert_validity();

    if (place_info.is_global())
        global_info = place_info;
    else
        branch_info[place_info.branch] = place_info;
}

vector<PlaceInfo> player::get_all_place_info(bool visited_only,
                                             bool dungeon_only) const
{
    vector<PlaceInfo> list;

    for (branch_iterator it; it; ++it)
    {
        if (visited_only && branch_info[it->id].num_visits == 0
            || dungeon_only && !is_connected_branch(*it))
        {
            continue;
        }
        list.push_back(branch_info[it->id]);
    }

    return list;
}

// Used for falling into traps and other bad effects, but is a slightly
// different effect from the player invokable ability.
bool player::do_shaft()
{
    if (!is_valid_shaft_level())
        return false;

    // Handle instances of do_shaft() being invoked magically when
    // the player isn't standing over a shaft.
    if (get_trap_type(pos()) != TRAP_SHAFT)
    {
        switch (grd(pos()))
        {
        case DNGN_FLOOR:
        case DNGN_OPEN_DOOR:
        // what's the point of this list?
        case DNGN_TRAP_MECHANICAL:
        case DNGN_TRAP_TELEPORT:
#if TAG_MAJOR_VERSION == 34
        case DNGN_TRAP_SHADOW:
        case DNGN_TRAP_SHADOW_DORMANT:
#endif
        case DNGN_TRAP_ALARM:
        case DNGN_TRAP_ZOT:
        case DNGN_TRAP_SHAFT:
        case DNGN_UNDISCOVERED_TRAP:
        case DNGN_ENTER_SHOP:
            break;

        default:
            return false;
        }

    }

    down_stairs(DNGN_TRAP_SHAFT);

    return true;
}

bool player::can_do_shaft_ability(bool quiet) const
{
    if (attribute[ATTR_HELD])
    {
        if (!quiet)
            mprf("You can't shaft yourself while %s.", held_status());
        return false;
    }

    switch (grd(pos()))
    {
    case DNGN_FLOOR:
    case DNGN_OPEN_DOOR:
        if (!is_valid_shaft_level())
        {
            if (!quiet)
                mpr("You can't shaft yourself on this level.");
            return false;
        }
        break;

    default:
        if (!quiet)
            mpr("You can't shaft yourself on this terrain.");
        return false;
    }

    return true;
}

// Like do_shaft, but forced by the player.
// It has a slightly different set of rules.
bool player::do_shaft_ability()
{
    if (can_do_shaft_ability(true))
    {
        mpr("A shaft appears beneath you!");
        down_stairs(DNGN_TRAP_SHAFT, true);
        return true;
    }
    else
    {
        canned_msg(MSG_NOTHING_HAPPENS);
        redraw_screen();
        return false;
    }
}

bool player::did_escape_death() const
{
    return escaped_death_cause != NUM_KILLBY;
}

void player::reset_escaped_death()
{
    escaped_death_cause = NUM_KILLBY;
    escaped_death_aux   = "";
}

void player::add_gold(int delta)
{
    set_gold(gold + delta);
}

void player::del_gold(int delta)
{
    set_gold(gold - delta);
}

void player::set_gold(int amount)
{
    ASSERT(amount >= 0);

    if (amount != gold)
    {
        const int old_gold = gold;
        gold = amount;
        shopping_list.gold_changed(old_gold, gold);

        // XXX: this might benefit from being in its own function
        if (you_worship(GOD_GOZAG))
        {
            for (const auto& power : get_god_powers(you.religion))
            {
                const int cost = get_gold_cost(power.abil);
                if (gold >= cost && old_gold < cost)
                    power.display(true, "You now have enough gold to %s.");
                else if (old_gold >= cost && gold < cost)
                    power.display(false, "You no longer have enough gold to %s.");
            }
        }
    }
}

void player::increase_duration(duration_type dur, int turns, int cap, const char *msg, source_type source)
{
    /*
    if (dur == DUR_EXHAUSTED)
    {
        const int sp_loss = turns;
        dec_sp(sp_loss);
        return;
    }
     */

    if (msg)
        mpr(msg);
    cap *= BASELINE_DELAY;

    duration[dur] += turns * BASELINE_DELAY;
    if (cap && duration[dur] > cap)
        duration[dur] = cap;

    if (you.rune_curse_active[RUNE_SPIDER])
    {
        switch(dur)
        {
            case DUR_ANCESTOR_DELAY:
            case DUR_BARBS:
            case DUR_CONF:
            case DUR_CORONA:
            case DUR_EXHAUSTED:
            case DUR_FLAYED:
            case DUR_FROZEN:
            case DUR_GRASPING_ROOTS:
            case DUR_LIQUID_FLAMES:
            case DUR_LOWERED_MR:
            case DUR_PARALYSIS:
            case DUR_PETRIFIED:
            case DUR_POISONING:
            case DUR_SILENCE:
            case DUR_SLOW:
            case DUR_VERTIGO:
                duration[dur] = duration[dur] * 5 / 4;
                break;
            default:
                break;
        }
    }
//    if (dur == DUR_BERSERK || dur == DUR_INVIS || dur == DUR_HASTE)
//        inc_sp(turns * 8);

    duration_source[dur] = source;
}

void player::set_duration(duration_type dur, int turns, int cap, const char *msg, source_type source)
{
    duration[dur] = 0;
    increase_duration(dur, turns, cap, msg, source);
}

void player::goto_place(const level_id &lid)
{
    where_are_you = static_cast<branch_type>(lid.branch);
    depth = lid.depth;
    ASSERT_RANGE(depth, 1, brdepth[where_are_you] + 1);
}

bool player::attempt_escape(int attempts)
{
    monster *themonst;

    if (!is_constricted())
        return true;

    themonst = monster_by_mid(constricted_by);
    ASSERT(themonst);
    escape_attempts += attempts;

    // player breaks free if (4+n)d(8+str/4) >= 5d(8+HD/4)
    if (roll_dice(4 + escape_attempts, 8 + div_rand_round(strength(), 4))
        >= roll_dice(5, 8 + div_rand_round(themonst->get_hit_dice(), 4)))
    {
        mprf("You escape %s's grasp.", themonst->name(DESC_THE, true).c_str());

        // Stun the monster to prevent it from constricting again right away.
        themonst->speed_increment -= 5;

        stop_being_constricted(true);

        return true;
    }
    else
    {
        mprf("Your attempt to break free from %s fails, but you feel that "
             "another attempt might succeed.",
             themonst->name(DESC_THE, true).c_str());
        turn_is_over = true;
        return false;
    }
}

void player::sentinel_mark(bool trap)
{
    if (duration[DUR_SENTINEL_MARK])
    {
        mpr("The mark upon you grows brighter.");
        increase_duration(DUR_SENTINEL_MARK, random_range(20, 40), 180);
    }
    else
    {
        mprf(MSGCH_WARN, "A sentinel's mark forms upon you.");
        increase_duration(DUR_SENTINEL_MARK, trap ? random_range(25, 40)
                                                  : random_range(35, 60),
                          250);
    }
}

bool player::made_nervous_by(const coord_def &p)
{
    if (form != TRAN_FUNGUS)
        return false;
    monster* mons = monster_at(p);
    if (mons && mons_is_threatening(mons))
        return false;
    for (monster_near_iterator mi(&you); mi; ++mi)
    {
        if (!mons_is_wandering(*mi)
            && !mi->asleep()
            && !mi->confused()
            && !mi->cannot_act()
            && mons_is_threatening(*mi)
            && !mi->wont_attack()
            && !mi->neutral())
        {
            return true;
        }
    }
    return false;
}

void player::weaken(actor *attacker, int pow)
{
    if (!duration[DUR_WEAK])
        mprf(MSGCH_WARN, "You feel your attacks grow feeble.");
    else
        mprf(MSGCH_WARN, "You feel as though you will be weak longer.");

    increase_duration(DUR_WEAK, pow + random2(pow + 3), 50);
}

/**
 * Check if the player is about to die from flight/form expiration.
 *
 * Check whether the player is on a cell which would be deadly if not for some
 * temporary condition, and if such condition is expiring. In that case, we
 * give a strong warning to the player. The actual message printing is done
 * by the caller.
 *
 * @param dur the duration to check for dangerous expiration.
 * @param p the coordinates of the cell to check. Defaults to player position.
 * @return whether the player is in immediate danger.
 */
bool need_expiration_warning(duration_type dur, dungeon_feature_type feat)
{
    if (!is_feat_dangerous(feat, true) || !dur_expiring(dur))
        return false;

    if (dur == DUR_FLIGHT)
        return true;
    else if (dur == DUR_TRANSFORMATION
             && (form_can_swim() || form_can_fly()))
    {
        return true;
    }
    return false;
}

bool need_expiration_warning(duration_type dur, coord_def p)
{
    return need_expiration_warning(dur, env.grid(p));
}

bool need_expiration_warning(dungeon_feature_type feat)
{
    return need_expiration_warning(DUR_FLIGHT, feat)
           || need_expiration_warning(DUR_TRANSFORMATION, feat);
}

bool need_expiration_warning(coord_def p)
{
    return need_expiration_warning(env.grid(p));
}

static string _constriction_description()
{
    string cinfo = "";
    vector<string> c_name;

    const int num_free_tentacles = you.usable_tentacles();
    if (num_free_tentacles)
    {
        cinfo += make_stringf("You have %d tentacle%s available for constriction.",
                              num_free_tentacles,
                              num_free_tentacles > 1 ? "s" : "");
    }
    // name of what this monster is constricted by, if any
    if (you.is_constricted())
    {
        if (!cinfo.empty())
            cinfo += "\n";

        cinfo += make_stringf("You are being %s by %s.",
                      you.held == HELD_MONSTER ? "held" : "constricted",
                      monster_by_mid(you.constricted_by)->name(DESC_A).c_str());
    }

    if (you.constricting && !you.constricting->empty())
    {
        for (const auto &entry : *you.constricting)
        {
            monster *whom = monster_by_mid(entry.first);
            ASSERT(whom);
            c_name.push_back(whom->name(DESC_A));
        }

        if (!cinfo.empty())
            cinfo += "\n";

        cinfo += "You are constricting ";
        cinfo += comma_separated_line(c_name.begin(), c_name.end());
        cinfo += ".";
    }

    return cinfo;
}

/**
 *   The player's radius of monster detection.
 *   @return   the radius in which a player can detect monsters.
**/
int player_monster_detect_radius()
{
    int radius = player_mutation_level(MUT_ANTENNAE) * 2;

    if (player_equip_unrand(UNRAND_BOOTS_ASSASSIN))
        radius = max(radius, 4);
    if (have_passive(passive_t::detect_montier))
        radius = max(radius, you.piety / 20);
    return min(radius, LOS_RADIUS);
}

/**
 * Return true if the player has angered Pandemonium by picking up or moving
 * the Orb of Zot.
 */
bool player_on_orb_run()
{
    return you.chapter == CHAPTER_ESCAPING
           || you.chapter == CHAPTER_ANGERED_PANDEMONIUM;
}

/**
 * Return true if the player has the Orb of Zot.
 * @return  True if the player has the Orb, false otherwise.
 */
bool player_has_orb()
{
    return you.chapter == CHAPTER_ESCAPING;
}

bool player::form_uses_xl() const
{
    // No body parts that translate in any way to something fisticuffs could
    // matter to, the attack mode is different. Plus, it's weird to have
    // users of one particular [non-]weapon be effective for this
    // unintentional form while others can just run or die. I believe this
    // should apply to more forms, too.  [1KB]
    return form == TRAN_WISP || form == TRAN_FUNGUS;
}

static int _get_device_heal_factor()
{
    // healing factor is expressed in thirds, so default is 3/3 -- 100%.
    int factor = 3;

    // start with penalties
    factor -= player_equip_unrand(UNRAND_VINES) ? 3 : 0;
    factor -= you.mutation[MUT_NO_DEVICE_HEAL];

    // then apply bonuses
    // Kryia's doubles device healing for non-deep dwarves, because deep dwarves
    // are abusive bastards.
    if (you.species != SP_DEEP_DWARF)
        factor *= player_equip_unrand(UNRAND_KRYIAS) ? 2 : 1;

    // make sure we don't turn healing negative.
    return max(0, factor);
}

bool player::can_device_heal()
{
    return _get_device_heal_factor() > 0;
}

int player::scale_device_healing(int healing_amount)
{
    return div_rand_round(healing_amount * _get_device_heal_factor(), 3);
}

// Lava orcs!
int temperature()
{
    return (int) you.temperature;
}

int temperature_last()
{
    return (int) you.temperature_last;
}

void temperature_check()
{
    // Whether to ignore caps on incrementing temperature
    bool ignore_cap = you.duration[DUR_BERSERK];

    // These numbers seem to work pretty well, but they're definitely experimental:
    int tension = get_tension(GOD_NO_GOD); // Raw tension

    // It would generally be better to handle this at the tension level and have temperature much more closely tied to tension.

    // For testing, but super handy for that!
    // mprf("Tension value: %d", tension);

    // Increment temp to full if you're in lava.
    if (feat_is_lava(env.grid(you.pos())) && you.ground_level())
    {
        // If you're already very hot, no message,
        // but otherwise it lets you know you're being
        // brought up to max temp.
        if (temperature() <= TEMP_FIRE)
            mpr("The lava instantly superheats you.");
        you.temperature = TEMP_MAX;
        ignore_cap = true;
        // Otherwise, your temperature naturally decays.
    }
    else
        temperature_decay();

    // Follow this up with 1 additional decrement each turn until
    // you're not hot enough to boil water.
    if (feat_is_water(env.grid(you.pos())) && you.ground_level()
        && temperature_effect(LORC_PASSIVE_HEAT))
    {
        temperature_decrement(1);

        for (adjacent_iterator ai(you.pos()); ai; ++ai)
        {
            const coord_def p(*ai);
            if (in_bounds(p)
                && !cloud_at(p)
                && !cell_is_solid(p)
                && one_chance_in(5))
            {
                place_cloud(CLOUD_STEAM, *ai, 2 + random2(5), &you);
            }
        }
    }

    // Next, add temperature from tension. Can override temperature loss from water!
    temperature_increment(tension);

    // Cap net temperature change to 1 per turn if no exceptions.
    float tempchange = you.temperature - you.temperature_last;
    if (!ignore_cap && tempchange > 1)
        you.temperature = you.temperature_last + 1;
    else if (tempchange < -1)
        you.temperature = you.temperature_last - 1;

    // Handle any effects that change with temperature.
    temperature_changed(tempchange);

    // Save your new temp as your new 'old' temperature.
    you.temperature_last = you.temperature;
}

void temperature_increment(float degree)
{
    // No warming up while you're exhausted!
    if (you.duration[DUR_EXHAUSTED])
        return;

    you.temperature += sqrt(degree);
    if (temperature() >= TEMP_MAX)
        you.temperature = TEMP_MAX;
}

void temperature_decrement(float degree)
{
    // No cooling off while you're angry!
    if (you.duration[DUR_BERSERK])
        return;

    you.temperature -= degree;
    if (temperature() <= TEMP_MIN)
        you.temperature = TEMP_MIN;
}

void temperature_changed(float change)
{
    // Arbitrary - how big does a swing in a turn have to be?
    float pos_threshold = .25;
    float neg_threshold = -1 * pos_threshold;

    // For INCREMENTS:

    // Check these no-nos every turn.
    if (you.temperature >= TEMP_WARM)
    {
        // Handles condensation shield, ozo's armour, icemail.
        // 10 => 100aut reduction in duration.
        maybe_melt_player_enchantments(BEAM_FIRE, 10);

        // Handled separately because normally heat doesn't affect this.
        if (you.form == TRAN_ICE_BEAST || you.form == TRAN_STATUE)
            untransform(false);
    }

    // Just reached the temp that kills off stoneskin.
    if (change > pos_threshold && temperature_tier(TEMP_WARM))
    {
        mprf(MSGCH_DURATION, "Your stony skin melts.");
        you.redraw_armour_class = true;
    }

    // Passive heat stuff.
    if (change > pos_threshold && temperature_tier(TEMP_FIRE))
        mprf(MSGCH_DURATION, "You're getting fired up.");

    // Heat aura stuff.
    if (change > pos_threshold && temperature_tier(TEMP_MAX))
    {
        mprf(MSGCH_DURATION, "You blaze with the fury of an erupting volcano!");
        invalidate_agrid(true);
    }

    // For DECREMENTS (reverse order):
    if (change < neg_threshold && temperature_tier(TEMP_MAX))
        mprf(MSGCH_DURATION, "The intensity of your heat diminishes.");

    if (change < neg_threshold && temperature_tier(TEMP_FIRE))
        mprf(MSGCH_DURATION, "You're cooling off.");

    // Cooled down enough for stoneskin to kick in again.
    if (change < neg_threshold && temperature_tier(TEMP_WARM))
    {
        mprf(MSGCH_DURATION, "Your skin cools and hardens.");
        you.redraw_armour_class = true;
    }

    // If we're in this function, temperature changed, anyways.
    you.redraw_temperature = true;

#ifdef USE_TILE
    init_player_doll();
#endif

    // Just do this every turn to be safe. Can be fixed later if there
    // any performance issues.
    invalidate_agrid(true);
}

void temperature_decay()
{
    temperature_decrement(you.temperature / 10);
}

// Just a helper function to save space. Returns true if a
// threshold was crossed.
bool temperature_tier (int which)
{
    if (temperature() > which && temperature_last() <= which)
        return true;
    else if (temperature() < which && temperature_last() >= which)
        return true;
    else
        return false;
}

bool temperature_effect(int which)
{
    if (you.species == SP_LAVA_ORC)
    	switch (which)
    	{
        case LORC_FIRE_RES_I:
            return true; // 1-15
        case LORC_STONESKIN:
            return temperature() < TEMP_WARM; // 1-8
//      case nothing, right now:
//            return (you.temperature >= TEMP_COOL && you.temperature < TEMP_WARM); // 5-8
        case LORC_LAVA_BOOST:
            return temperature() >= TEMP_WARM; // 9-10
        case LORC_FIRE_RES_II:
            return temperature() >= TEMP_WARM; // 9-15
        case LORC_FIRE_RES_III:
//        case LORC_FIRE_BOOST:
        case LORC_COLD_VULN:
            return temperature() >= TEMP_HOT; // 11-15
        case LORC_PASSIVE_HEAT:
            return temperature() >= TEMP_FIRE; // 13-15
        case LORC_HEAT_AURA:
            if (you_worship(GOD_BEOGH))
                return false;
            // Deliberate fall-through.
        case LORC_NO_SCROLLS:
            return temperature() >= TEMP_MAX; // 15

        default:
            return false;
    	}
    else
    	return false;
}

int temperature_colour(int temp)
{
    return (temp > TEMP_FIRE) ? LIGHTRED  :
           (temp > TEMP_HOT)  ? RED       :
           (temp > TEMP_WARM) ? YELLOW    :
           (temp > TEMP_ROOM) ? WHITE     :
           (temp > TEMP_COOL) ? LIGHTCYAN :
           (temp > TEMP_COLD) ? LIGHTBLUE : BLUE;
}

string temperature_string(int temp)
{
    return (temp > TEMP_FIRE) ? "lightred"  :
           (temp > TEMP_HOT)  ? "red"       :
           (temp > TEMP_WARM) ? "yellow"    :
           (temp > TEMP_ROOM) ? "white"     :
           (temp > TEMP_COOL) ? "lightcyan" :
           (temp > TEMP_COLD) ? "lightblue" : "blue";
}

string temperature_text(int temp)
{
    switch (temp)
    {
        case TEMP_MIN:
            return "rF+; racial bonus to AC";
        case TEMP_COOL:
            return "";
        case TEMP_WARM:
            return "rF++; lava magic boost; Stony skin melts";
        case TEMP_HOT:
            return "rF+++; rC-; move faster in Power mode";
        case TEMP_FIRE:
            return "Burn attackers";
        case TEMP_MAX:
            return "All above effects; fire aura; can't read";
        default:
            return "";
    }
}

void player_open_door(coord_def doorpos)
{
    // Finally, open the closed door!
    set<coord_def> all_door;
    find_connected_identical(doorpos, all_door);
    const char *adj, *noun;
    get_door_description(all_door.size(), &adj, &noun);

    const string door_desc_adj  =
        env.markers.property_at(doorpos, MAT_ANY, "door_description_adjective");
    const string door_desc_noun =
        env.markers.property_at(doorpos, MAT_ANY, "door_description_noun");
    if (!door_desc_adj.empty())
        adj = door_desc_adj.c_str();
    if (!door_desc_noun.empty())
        noun = door_desc_noun.c_str();

    if (!you.confused())
    {
        string door_open_prompt =
            env.markers.property_at(doorpos, MAT_ANY, "door_open_prompt");

        bool ignore_exclude = false;

        if (!door_open_prompt.empty())
        {
            door_open_prompt += " (y/N)";
            if (!yesno(door_open_prompt.c_str(), true, 'n', true, false))
            {
                if (is_exclude_root(doorpos))
                    canned_msg(MSG_OK);
                else
                {
                    if (yesno("Put travel exclusion on door? (Y/n)",
                              true, 'y'))
                    {
                        // Zero radius exclusion right on top of door.
                        set_exclude(doorpos, 0);
                    }
                }
                interrupt_activity(AI_FORCE_INTERRUPT);
                return;
            }
            ignore_exclude = true;
        }

        if (!ignore_exclude && is_exclude_root(doorpos))
        {
            string prompt = make_stringf("This %s%s is marked as excluded! "
                                         "Open it anyway?", adj, noun);

            if (!yesno(prompt.c_str(), true, 'n', true, false))
            {
                canned_msg(MSG_OK);
                interrupt_activity(AI_FORCE_INTERRUPT);
                return;
            }
        }
    }

    const int skill = 8 + you.skill_rdiv(SK_STEALTH, 4, 3);

    string berserk_open = env.markers.property_at(doorpos, MAT_ANY,
                                                  "door_berserk_verb_open");
    string berserk_adjective = env.markers.property_at(doorpos, MAT_ANY,
                                                       "door_berserk_adjective");
    string door_open_creak = env.markers.property_at(doorpos, MAT_ANY,
                                                     "door_noisy_verb_open");
    string door_airborne = env.markers.property_at(doorpos, MAT_ANY,
                                                   "door_airborne_verb_open");
    string door_open_verb = env.markers.property_at(doorpos, MAT_ANY,
                                                    "door_verb_open");

    if (you.berserk())
    {
        // XXX: Better flavour for larger doors?
        if (silenced(you.pos()))
        {
            if (!berserk_open.empty())
            {
                berserk_open += ".";
                mprf(berserk_open.c_str(), adj, noun);
            }
            else
                mprf("The %s%s flies open!", adj, noun);
        }
        else
        {
            if (!berserk_open.empty())
            {
                if (!berserk_adjective.empty())
                    berserk_open += " " + berserk_adjective;
                else
                    berserk_open += ".";
                mprf(MSGCH_SOUND, berserk_open.c_str(), adj, noun);
            }
            else
                mprf(MSGCH_SOUND, "The %s%s flies open with a bang!", adj, noun);
            noisy(15, you.pos());
        }
    }
    else if (one_chance_in(skill) && !silenced(you.pos()))
    {
        if (!door_open_creak.empty())
            mprf(MSGCH_SOUND, door_open_creak.c_str(), adj, noun);
        else
        {
            mprf(MSGCH_SOUND, "As you open the %s%s, it creaks loudly!",
                 adj, noun);
        }
        noisy(10, you.pos());
    }
    else
    {
        const char* verb;
        if (you.airborne())
        {
            if (!door_airborne.empty())
                verb = door_airborne.c_str();
            else
                verb = "You reach down and open the %s%s.";
        }
        else
        {
            if (!door_open_verb.empty())
               verb = door_open_verb.c_str();
            else
               verb = "You open the %s%s.";
        }

        mprf(verb, adj, noun);
    }

    vector<coord_def> excludes;
    for (const auto &dc : all_door)
    {
        // Even if some of the door is out of LOS, we want the entire
        // door to be updated. Hitting this case requires a really big
        // door!
        if (env.map_knowledge(dc).seen())
        {
            env.map_knowledge(dc).set_feature(DNGN_OPEN_DOOR);
#ifdef USE_TILE
            env.tile_bk_bg(dc) = TILE_DNGN_OPEN_DOOR;
#endif
        }
        if (grd(dc) == DNGN_RUNED_DOOR)
            opened_runed_door();
        grd(dc) = DNGN_OPEN_DOOR;
        set_terrain_changed(dc);
        dungeon_events.fire_position_event(DET_DOOR_OPENED, dc);
        if (is_excluded(dc))
            excludes.push_back(dc);
    }

    update_exclusion_los(excludes);
    viewwindow();
    you.turn_is_over = true;
}

void player_close_door(coord_def doorpos)
{
    // Finally, close the opened door!
    string berserk_close = env.markers.property_at(doorpos, MAT_ANY,
                                                "door_berserk_verb_close");
    const string berserk_adjective = env.markers.property_at(doorpos, MAT_ANY,
                                                "door_berserk_adjective");
    const string door_close_creak = env.markers.property_at(doorpos, MAT_ANY,
                                                "door_noisy_verb_close");
    const string door_airborne = env.markers.property_at(doorpos, MAT_ANY,
                                                "door_airborne_verb_close");
    const string door_close_verb = env.markers.property_at(doorpos, MAT_ANY,
                                                "door_verb_close");
    const string door_desc_adj  = env.markers.property_at(doorpos, MAT_ANY,
                                                "door_description_adjective");
    const string door_desc_noun = env.markers.property_at(doorpos, MAT_ANY,
                                                "door_description_noun");
    set<coord_def> all_door;
    find_connected_identical(doorpos, all_door);
    const char *adj, *noun;
    get_door_description(all_door.size(), &adj, &noun);
    const string waynoun_str = make_stringf("%sway", noun);
    const char *waynoun = waynoun_str.c_str();

    if (!door_desc_adj.empty())
        adj = door_desc_adj.c_str();
    if (!door_desc_noun.empty())
    {
        noun = door_desc_noun.c_str();
        waynoun = noun;
    }

    for (const coord_def& dc : all_door)
    {
        if (monster* mon = monster_at(dc))
        {
            const bool mons_unseen = !you.can_see(*mon);
            if (mons_unseen || mons_is_object(mon->type))
            {
                mprf("Something is blocking the %s!", waynoun);
                // No free detection!
                if (mons_unseen)
                    you.turn_is_over = true;
            }
            else
                mprf("There's a creature in the %s!", waynoun);
            return;
        }

        /*
        if (igrd(dc) != NON_ITEM)
        {
            mprf("There's something blocking the %s.", waynoun);
            return;
        }
        */

        if (you.pos() == dc)
        {
            mprf("There's a thick-headed creature in the %s!", waynoun);
            return;
        }
    }

    const int skill = 8 + you.skill_rdiv(SK_STEALTH, 4, 3);

    if (you.berserk())
    {
        if (silenced(you.pos()))
        {
            if (!berserk_close.empty())
            {
                berserk_close += ".";
                mprf(berserk_close.c_str(), adj, noun);
            }
            else
                mprf("You slam the %s%s shut!", adj, noun);
        }
        else
        {
            if (!berserk_close.empty())
            {
                if (!berserk_adjective.empty())
                    berserk_close += " " + berserk_adjective;
                else
                    berserk_close += ".";
                mprf(MSGCH_SOUND, berserk_close.c_str(), adj, noun);
            }
            else
            {
                mprf(MSGCH_SOUND, "You slam the %s%s shut with a bang!",
                                  adj, noun);
            }

            noisy(15, you.pos());
        }
    }
    else if (one_chance_in(skill) && !silenced(you.pos()))
    {
        if (!door_close_creak.empty())
            mprf(MSGCH_SOUND, door_close_creak.c_str(), adj, noun);
        else
        {
            mprf(MSGCH_SOUND, "As you close the %s%s, it creaks loudly!",
                              adj, noun);
        }

        noisy(10, you.pos());
    }
    else
    {
        const char* verb;
        if (you.airborne())
        {
            if (!door_airborne.empty())
                verb = door_airborne.c_str();
            else
                verb = "You reach down and close the %s%s.";
        }
        else
        {
            if (!door_close_verb.empty())
                verb = door_close_verb.c_str();
            else
                verb = "You close the %s%s.";
        }

        mprf(verb, adj, noun);
    }

    vector<coord_def> excludes;
    for (const coord_def& dc : all_door)
    {
        // Once opened, formerly runed doors become normal doors.
        grd(dc) = DNGN_CLOSED_DOOR;
        set_terrain_changed(dc);
        dungeon_events.fire_position_event(DET_DOOR_CLOSED, dc);

        // Even if some of the door is out of LOS once it's closed
        // (or even if some of it is out of LOS when it's open), we
        // want the entire door to be updated.
        if (env.map_knowledge(dc).seen())
        {
            env.map_knowledge(dc).set_feature(DNGN_CLOSED_DOOR);
#ifdef USE_TILE
            env.tile_bk_bg(dc) = TILE_DNGN_CLOSED_DOOR;
#endif
        }
        if (is_excluded(dc))
            excludes.push_back(dc);
    }

    update_exclusion_los(excludes);
    you.turn_is_over = true;
}

/**
 * Return a string describing the player's hand(s) taking a given verb.
 *
 * @param plural_verb    A plural-agreeing verb. ("Smoulders", "are", etc.)
 * @return               A string describing the action.
 *                       E.g. "tentacles smoulder", "paw is", etc.
 */
string player::hands_verb(const string &plural_verb) const
{
    bool plural;
    const string hand = hand_name(true, &plural);
    return hand + " " + conjugate_verb(plural_verb, plural);
}

// Is this a character that would not normally have a preceding space when
// it follows a word?
static bool _is_end_punct(char c)
{
    switch (c)
    {
    case ' ': case '.': case '!': case '?':
    case ',': case ':': case ';': case ')':
        return true;
    }
    return false;
}

/**
 * Return a string describing the player's hand(s) (or equivalent) taking the
 * given action (verb).
 *
 * @param plural_verb   The plural-agreeing verb corresponding to the action to
 *                      take. E.g., "smoulder", "glow", "gain", etc.
 * @param object        The object or predicate complement of the action,
 *                      including any sentence-final punctuation. E.g. ".",
 *                      "new energy.", etc.
 * @return              A string describing the player's hands taking the
 *                      given action. E.g. "Your tentacle gains new energy."
 */
string player::hands_act(const string &plural_verb,
                         const string &object) const
{
    const bool space = !object.empty() && !_is_end_punct(object[0]);
    return "Your " + hands_verb(plural_verb) + (space ? " " : "") + object;
}

/**
 * Possibly drop a point of bone armour (from Cigotuvi's Embrace) when hit,
 * or over time.
 *
 * Chance of losing a point of ac/sh increases with current number of corpses
 * (ATTR_BONE_ARMOUR). Each added corpse increases the chance of losing a bit
 * by 5/4x. (So ten corpses are a 9x chance, twenty are 87x...)
 *
 * Base chance is 1/500 (per aut) - 2% per turn, 63% within 50 turns.
 * At 10 corpses, that becomes a 17% per-turn chance, 61% within 5 turns.
 * At 20 corpses, that's 20% per-aut, 90% per-turn...
 *
 * Getting hit/blocking has a higher (BONE_ARMOUR_HIT_RATIO *) chance;
 * at BONE_ARMOUR_HIT_RATIO = 50, that's 10% at one corpse, 30% at five,
 * 90% at ten...
 *
 * @param trials  The number of times to potentially shed armour.
 */
void player::maybe_degrade_bone_armour(int trials)
{
    if (attribute[ATTR_BONE_ARMOUR] <= 0)
        return;

    const int base_denom = 50 * BASELINE_DELAY;
    int denom = base_denom;
    for (int i = 1; i < attribute[ATTR_BONE_ARMOUR]; ++i)
        denom = div_rand_round(denom * 4, 5);

    const int degraded_armour = binomial(trials, 1, denom);
    dprf("degraded armour? (%d armour, %d/%d in %d trials): %d",
         attribute[ATTR_BONE_ARMOUR], 1, denom, trials, degraded_armour);
    if (degraded_armour <= 0)
        return;

    you.attribute[ATTR_BONE_ARMOUR]
        = max(0, you.attribute[ATTR_BONE_ARMOUR] - degraded_armour);

    if (!you.attribute[ATTR_BONE_ARMOUR])
        mpr("The last of your corpse armour falls away.");
    else
        for (int i = 0; i < degraded_armour; ++i)
            mpr("A chunk of your corpse armour falls away.");

    redraw_armour_class = true;
}

int player::inaccuracy() const
{
    int degree = 0;
    if (wearing(EQ_AMULET, AMU_INACCURACY))
        degree++;
    if (player_mutation_level(MUT_MISSING_EYE))
        degree++;
    return degree;
}

bool can_use(const item_def &item)
{
    if (you.species == SP_TENGU && item.base_type == OBJ_JEWELLERY && item.sub_type < NUM_RINGS)
    	return false;

    return true;
}

/**
 * Handle effects that occur after the player character stops berserking.
 */
void player_end_berserk()
{
    // Sometimes berserk leaves us physically drained.
    //
    // Chance of passing out:
    //     - mutation gives a large plus in order to try and
    //       avoid the mutation being a "death sentence" to
    //       certain characters.

    if (you.berserk_penalty != NO_BERSERK_PENALTY
        && one_chance_in(10 + player_mutation_level(MUT_BERSERK) * 25))
    {
        // Note the beauty of Trog!  They get an extra save that's at
        // the very least 20% and goes up to 100%.
        if (have_passive(passive_t::extend_berserk)
            && x_chance_in_y(you.piety, piety_breakpoint(5)))
        {
            mpr("Trog's vigour flows through your veins.");
        }
        else
        {
            mprf(MSGCH_WARN, "You pass out from exhaustion.");
            you.increase_duration(DUR_PARALYSIS, roll_dice(1,4));
            you.stop_constricting_all();
        }
    }

    /* This message already happens when stamina gets very low
    if (!you.duration[DUR_PARALYSIS] && !you.petrified())
        mprf(MSGCH_WARN, "You are exhausted.");
        */

    if (you.species == SP_LAVA_ORC)
        mpr("You feel less hot-headed.");

    // This resets from an actual penalty or from NO_BERSERK_PENALTY.
    you.berserk_penalty = 0;

    const int dur = 12 + roll_dice(2, 12);
    // For consistency with slow give exhaustion 2 times the nominal
    // duration.
    dec_sp(50, true);
    you.duration[DUR_EXHAUSTED] = dur * 10;

    notify_stat_change(STAT_STR, -5, true);

    // Don't trigger too many hints mode messages.
    const bool hints_slow = Hints.hints_events[HINT_YOU_ENCHANTED];
    Hints.hints_events[HINT_YOU_ENCHANTED] = false;

    /* This isn't necessary since we tank stamina
    slow_player(dur);
     */

    make_hungry(BERSERK_NUTRITION, true);
    you.hunger = max(HUNGER_STARVING - 100, you.hunger);

    // 1KB: No berserk healing.
    calc_hp();

    learned_something_new(HINT_POSTBERSERK);
    Hints.hints_events[HINT_YOU_ENCHANTED] = hints_slow;
    you.redraw_quiver = true; // Can throw again.
}

const int get_max_exp_level()
{
    if (Options.level_27_cap)
        return 27;
    return MAX_EXP_LEVEL;
}

const int get_max_skill_level()
{
    if (Options.level_27_cap)
        return 27;
    return MAX_SKILL_LEVEL;
}

void summoned_monster_died(mid_t mons, int mp_freeze, bool natural_death)
{
    unfreeze_summons_mp(mp_freeze);
    int mp_recovered = mp_freeze;
    if (natural_death)
    {
        mp_recovered = div_rand_round(mp_recovered, 3);
    }
    inc_mp(mp_recovered);

    remove_from_summoned(mons);
}

void remove_from_summoned(mid_t mid)
{
    for (unsigned int i = 0; i < you.summoned.size(); i++)
    {
        if (you.summoned[i] == mid)
        {
            you.summoned[i] = MID_NOBODY;
            break;
        }
    }
}

int player_summon_count()
{
    int count = 0;
    for (unsigned int i = 0; i < you.summoned.size(); i++)
    {
        if (you.summoned[i] != MID_NOBODY)
            count++;
    }

    return count;
}

bool player_summoned_monster(spell_type spell, monster* mons, bool first, int freeze_cost)
{
    bool success = true;
    int cost = freeze_cost;
    if (cost == -1)
        cost = spell_mp_freeze(spell);

    if (!first)
        cost = div_rand_round(cost, 3);

    mons->mp_freeze = cost;
    while (true)
    {
        if (!freeze_summons_mp(cost))
        {
            mpr("You don't have enough magic to support more minions of this magnitude.");
            success = false;
            break;
        }

        int open_slot = -1;
        for (unsigned int i = 0; i < you.summoned.size(); i++)
        {
            if (you.summoned[i] == MID_NOBODY)
            {
                open_slot = i;
                break;
            }
        }

        if (open_slot == -1)
        {
            mpr("Your mind can't handle so many minions at once.");
            success = false;
        }
        else
        {
            you.summoned[open_slot] = mons->mid;
        }

        break;
    }

    return success;
}

// triggered for any ranged or melee attack
void player_attacked_something(int sp_cost)
{
    player_was_offensive();
}

int generic_action_delay(const int skill, const int base, const action_delay_type type)
{
    const int dex = (you.dex(true) - 10) * 10;
    const int min_delay_reached_at = 60;
    // 100 is full amount, 80 = 80% of original
    const int global_reduction = 100;

    const int factor = (min_delay_reached_at - 10) * 10;

    int benefit = skill + dex;

    benefit = max(0, benefit);
    benefit = min(factor, benefit);

    if (type == ACTION_DELAY_MAX)
        benefit = 0;
    if (type == ACTION_DELAY_MIN)
        benefit = factor;

    int delay = global_reduction * base * (factor * 3 - benefit * 2) / (factor * 3) / 10;
    delay = (player_attack_delay_modifier(delay) + 5) / 10;
    return delay;
}

int spell_cast_delay(const action_delay_type type)
{
    const int skill = you.skill(SK_SPELLCASTING, 10);
    const int base = 20;

    int delay = generic_action_delay(skill, base);

    if (you.wearing(EQ_AMULET, AMU_QUICK_CAST))
        delay = (delay + 1) / 2;

    const int quick_cast = player_mutation_level(MUT_QUICK_CASTING);
    if (quick_cast)
        delay = delay * (4 - quick_cast) / 4;

    return delay * you.time_taken / 10;
}

// When any kind of magic spell is cast by the player
void player_used_magic(int mp_cost, spell_type spell)
{
    player_was_offensive();

    you.time_taken = spell_cast_delay();
}

void player_evoked_something()
{
    player_was_offensive();
}

void player_moved()
{
    double movement_cost = 0;

    if (you.peace < 200 && in_quick_mode())
        movement_cost += 20;

    if (you.peace < 50 && you.airborne() && you.cancellable_flight())
        movement_cost += 20;

    const int armour_penalty = you.adjusted_body_armour_penalty(10);
    const int shield_penalty = you.adjusted_shield_penalty(10);
    const int total_penalty = armour_penalty + shield_penalty;

    double slow_down = 0;
    if (movement_cost > 0 && total_penalty > 0)
    {
        slow_down = log2((total_penalty + 10) / 10.0);
        movement_cost += slow_down * 10;
    }

    movement_cost = div_rand_round(movement_cost, 10);
    dec_sp(movement_cost, false, true);
}

// factor reduces failure chance by the given percentage (or success chance if fail < 50%)
bool _spell_fails(spell_type spell, int factor)
{
    int fail = raw_spell_fail(spell);
    const bool flip = fail > 50;
    if (flip)
        fail = 100 - fail;
    fail = fail * factor / 100;
    if (flip)
        fail = 100 - fail;

    const bool failed = x_chance_in_y(fail + 1, 100);
    return failed;
}

void player_summon_was_shot_through(monster* mon)
{
    const spell_type spell = mon->summoned_by_spell;
    if (spell && spell != SPELL_NO_SPELL && _spell_fails(spell, 100))
    {
        mpr("Your missile disrupts your summoned creature.");
        unsummon(mon);
    }
}

void player_was_offensive()
{
    if (you.current_form_spell != SPELL_NO_SPELL)
    {
        if (_spell_fails(you.current_form_spell, 100))
        {
            you.current_form_spell_failure++;
            if (you.current_form_spell_failure == 2)
                mprf(MSGCH_WARN, "Your form is beginning to unravel.");

            if (you.current_form_spell_failure == 3)
                mprf(MSGCH_WARN, "You can't maintain your form for much longer!");

            if (you.current_form_spell_failure > 3)
                untransform();
        }
    }
}

void player_before_long_safe_action()
{
    /* no longer needed since quick mode doesn't use stamina when monsters aren't around
    if (in_quick_mode())
        set_quick_mode(false);
     */
}

void player_after_long_safe_action(int turns)
{
}

void player_after_each_turn()
{
    if (you.current_form_spell_failure && you.peace >= 200 && you.current_form_spell_failure > 0)
    {
        mpr("Your form becomes more stable.");
        you.current_form_spell_failure = 0;
    }

    crawl_state.danger_mode_counter = max(0, crawl_state.danger_mode_counter - 1);

    if (Options.exertion_disabled)
    {
        if (you.exertion != EXERT_NORMAL)
            you.exertion = EXERT_NORMAL;
    }
}

int spell_mp_cost(spell_type which_spell)
{
    int cost = spell_hunger(which_spell, false) / 10;

    cost = max(cost, 2);

    if (you.duration[DUR_CHANNELING]
        || spell_produces_minion(which_spell)
        )
        cost = 0;

    if (is_self_transforming_spell(which_spell))
        cost *= 3;

    if (you.exertion != EXERT_NORMAL)
        cost *= 2;

    if (you.rune_curse_active[RUNE_SNAKE])
        cost = cost * 5 / 4;

    return cost;
}

int spell_mp_freeze(spell_type which_spell)
{
    int cost = 0;
    if (spell_produces_minion(which_spell))
    {
        cost = spell_hunger(which_spell, false, 200) / 3;

        cost = max(cost, 5);

        if (you.exertion != EXERT_NORMAL && you.peace < 50)
            cost *= 2;
    }

    if (you.rune_curse_active[RUNE_SNAKE])
        cost = cost * 5 / 4;

    return cost;
}

int weapon_sp_cost(const item_def* weapon, const item_def* ammo)
{
    int weight = weapon ? max(1, property(*weapon, PWPN_WEIGHT)) : 3;
    if (ammo && (!weapon || !ammo->launched_by(*weapon)))
        weight = property(*ammo, PWPN_WEIGHT);

    const int strength = (you.strength(true) - 10) * 10;
    const int fighting = you.skill(SK_FIGHTING, 10);

    // may be negative
    const int benefit = strength + fighting;

    double sp_cost = fpow(weight, 15, 16, benefit / 10.0);

    if (you.exertion != EXERT_NORMAL)
    {
        sp_cost *= 2;
        sp_cost = max(3, (int)sp_cost);
    }

    if (weapon && get_weapon_brand(*weapon) == SPWPN_LIGHT)
        sp_cost /= 2;

    if (you.rune_curse_active[RUNE_SNAKE])
        sp_cost = sp_cost * 5 / 4;

    sp_cost = max(1, (int)sp_cost);

    return sp_cost;
}

const int base_factor = 100;

int _difficulty_mode_adder()
{
    int x = 0;

    switch(crawl_state.difficulty)
    {
        case DIFFICULTY_EASY:
            x = 1;
            break;
        case DIFFICULTY_STANDARD:
            x = 0;
            break;
        case DIFFICULTY_CHALLENGE:
            x = -1;
            break;
        case DIFFICULTY_NIGHTMARE:
            x = -2;
            break;
        default:
            // should not be possible
            x = 0;
            break;
    }

    if (Options.exertion_disabled)
        x += 1;

    return x;
}

// all standard attributes are multipled by the base factor and then
// divided by this value. So if this function returns 100 (the current
// base_factor), that means that the normal mode damage (for example)
// is the same as vanilla crawl damage.
int _difficulty_mode_multiplier()
{
    int x = 100;

    switch(crawl_state.difficulty)
    {
        case DIFFICULTY_EASY:
            x = 110;
            break;
        case DIFFICULTY_STANDARD:
            x = 100;
            break;
        case DIFFICULTY_CHALLENGE:
            x = 90;
            break;
        case DIFFICULTY_NIGHTMARE:
            x = 80;
            break;
        default:
            // should not be possible
            x = 100;
            break;
    }

    if (Options.exertion_disabled)
    {
        // bring challenge mode back to baseline if player isn't
        // using exertion modes
        x += 10;
    }

    return x;
}

int player_tohit_modifier(int tohit, int range)
{
    ASSERT(range <= LOS_MAX_RANGE);

    if (tohit >= AUTOMATIC_HIT)
        return tohit;

    if (you.duration[DUR_PORTAL_PROJECTILE])
        range = 1;

    tohit += _difficulty_mode_adder();

    if (range > 1)
        tohit = tohit - 3 * range;

    if (player_is_very_tired(true))
        tohit = tohit - 5;
    else if (you.exertion == EXERT_FOCUS)
        tohit = tohit + 10;

    return tohit;
}

int player_damage_modifier(int damage, bool silent, const int range)
{
    damage *= _difficulty_mode_multiplier();

    // worst case, range 7, gives 80% of original damage
    /*
    if (range > 1)
        damage = damage * (30 - range + 1) / 30;
        */

    if (player_is_very_tired(true))
        damage = damage * 4 / 5;
    else if (you.exertion == EXERT_POWER)
        damage = damage * 4 / 3 + 20;

    return damage / base_factor;
}

int player_attack_delay_modifier(int attack_delay)
{
    attack_delay *= base_factor;

    if (player_is_very_tired(true))
        attack_delay = attack_delay * 7 / 6;
    else if (you.exertion == EXERT_POWER)
        attack_delay = attack_delay * 5 / 6 - 50;

    return attack_delay / _difficulty_mode_multiplier();
}

double player_spellpower_modifier(double spellpower)
{
    spellpower *= _difficulty_mode_multiplier();

    if (player_is_very_tired(true))
        spellpower = spellpower * 3 / 4;
    else if (you.exertion == EXERT_POWER)
        spellpower = spellpower * 4 / 3 + 100;

    return spellpower / base_factor;
}

int player_stealth_modifier(int stealth)
{
    stealth *= _difficulty_mode_multiplier();

    if (player_is_very_tired(true))
        stealth = stealth * 2 / 3;
    else if (you.exertion == EXERT_FOCUS)
        stealth = stealth * 4 / 3 + 500;

    return stealth / base_factor;
}

int player_ev_modifier(int ev)
{
    ev *= _difficulty_mode_multiplier();

    if (player_is_very_tired(true))
        ev = ev * 5 / 6;
    else if (you.exertion == EXERT_FOCUS && ev > 0)
        ev = ev * 6 / 5 + 200;

    return ev / base_factor;
}

int player_ac_modifier(int ac)
{
    ac *= _difficulty_mode_multiplier();

    if (player_is_very_tired(true))
        ac = ac * 5 / 6;
    else if (you.exertion == EXERT_FOCUS && ac > 0)
        ac = ac * 6 / 5 + 10000;

    return ac / base_factor;
}

int player_sh_modifier(int sh)
{
    sh *= _difficulty_mode_multiplier();

    if (player_is_very_tired(true))
        sh = sh * 5 / 6;
    else if (you.exertion == EXERT_FOCUS && sh > 0)
        sh = sh * 6 / 5 + 500;

    return sh / base_factor;
}

int player_mr_modifier(int mr)
{
    mr *= _difficulty_mode_multiplier();

    if (player_is_very_tired(true))
        mr = mr * 4 / 5;
    else if (you.exertion == EXERT_FOCUS && mr > 0)
        mr = mr * 5 / 4 + 1000;

    return mr / base_factor;
}

int player_spellsuccess_modifier(int force)
{
    switch(crawl_state.difficulty)
    {
        case DIFFICULTY_EASY:
            force -= 1;
            break;
        case DIFFICULTY_STANDARD:
            force -= 2;
            break;
        case DIFFICULTY_CHALLENGE:
            force -= 3;
            break;
        case DIFFICULTY_NIGHTMARE:
            force -= 4;
            break;
        default:
            // should not be possible
            force -= 2;
            break;
    }

    if (player_is_very_tired(true))
        force -= 5;
    else if (you.exertion == EXERT_FOCUS || Options.exertion_disabled)
        force += 5;

    return force;
}

int player_item_gen_modifier(int item_count)
{
    int x;
    switch(crawl_state.difficulty)
    {
        case DIFFICULTY_EASY:
            x = 120;
            break;
        case DIFFICULTY_STANDARD:
            x = 100;
            break;
        case DIFFICULTY_CHALLENGE:
            x = 80;
            break;
        case DIFFICULTY_NIGHTMARE:
            x = 60;
            break;
        default:
            // should not be possible
            x = 100;
            break;
    }

    if (you.rune_curse_active[RUNE_VAULTS])
    {
        x = x * 3 / 4;
    }

    item_count = div_rand_round(item_count * x, 100);

    return item_count;
}

// used for pool sizes. Generic way to scale something based on difficulty
int player_pool_modifier(int amount)
{
    // bypass this for now
    return amount;

    int percent = 100;

    switch (crawl_state.difficulty)
    {
        case DIFFICULTY_EASY:
            percent = 110;
            break;
        case DIFFICULTY_STANDARD:
            percent = 100;
            break;
        case DIFFICULTY_CHALLENGE:
            percent = 90;
            break;
        case DIFFICULTY_NIGHTMARE:
            percent = 80;
            break;
        default:
            // should not be possible
            break;
    }

    return amount * percent / 100;
}

int player_monster_gen_modifier(int amount)
{
    int percent = 100;

    switch (crawl_state.difficulty)
    {
        case DIFFICULTY_EASY:
            percent = 80;
            break;
        case DIFFICULTY_STANDARD:
            percent = 90;
            break;
        case DIFFICULTY_CHALLENGE:
            percent = 100;
            break;
        case DIFFICULTY_NIGHTMARE:
            percent = 110;
            break;
        default:
            // should not be possible
            break;
    }

    if (you.rune_curse_active[RUNE_ABYSSAL])
        percent += 25;

    return amount * percent / 100;
}

int player_potion_recharge_percent()
{
    int percent = 60;

    switch (crawl_state.difficulty)
    {
        case DIFFICULTY_EASY:
            percent = 80;
            break;
        case DIFFICULTY_STANDARD:
            percent = 60;
            break;
        case DIFFICULTY_CHALLENGE:
            percent = 40;
            break;
        case DIFFICULTY_NIGHTMARE:
            percent = 20;
            break;
        default:
            // should not be possible
            break;
    }

    if (you.species == SP_DJINNI)
        percent /= 3;

    return percent;
}

int player_pre_ouch_modifier(int damage)
{
    // global monster damage reduction
    damage = div_rand_round(damage * 2, 3);

    return damage;
}

// reduce damage to player if it has exceeded protection thresholds (to avoid 1 hit kills for example)
int player_ouch_modifier(int damage, bool skip_details)
{
    int percentage_allowed = 100;

    switch (crawl_state.difficulty)
    {
        case DIFFICULTY_EASY:
            percentage_allowed = 20;
            break;
        case DIFFICULTY_STANDARD:
            percentage_allowed = 30;
            break;
        case DIFFICULTY_CHALLENGE:
            percentage_allowed = 40;
            break;
        case DIFFICULTY_NIGHTMARE:
            percentage_allowed = 50;
            break;
        default:
            // should not be possible
            break;
    }

    if (Options.disable_instakill_protection)
        percentage_allowed = 1000;

    const int max_damage_allowed_per_turn = get_hp_max() * percentage_allowed / 100;
    const int damage_left = max_damage_allowed_per_turn - you.turn_damage;

    int new_damage = min(damage, damage_left);
    new_damage = max(0, new_damage);

    if (damage > new_damage)
        mprf("You were prevented from receiving too much damage! (%d -> %d)", damage, new_damage);
    else
    {
        if (new_damage > 0 && !skip_details)
            mprf("(hp-%d)", new_damage);
    }

    return new_damage;
}

int player_max_stat_loss_allowed(stat_type stat)
{
    const int max_stat = you.max_stat(stat);
    int max_stat_loss = max_stat;

    int percentage_allowed = 20;
    switch (crawl_state.difficulty)
    {
        case DIFFICULTY_EASY:
            percentage_allowed = 20;
            break;
        case DIFFICULTY_STANDARD:
            percentage_allowed = 30;
            break;
        case DIFFICULTY_CHALLENGE:
            percentage_allowed = 40;
            break;
        case DIFFICULTY_NIGHTMARE:
            percentage_allowed = 50;
            break;
        default:
            // should not be possible
            break;
    }

    max_stat_loss = percentage_allowed * max_stat_loss / 100;
    max_stat_loss = min(max_stat_loss, max_stat - (9 - crawl_state.difficulty * 2));

    return max_stat_loss;
}

void player_update_last_be_hit_chance(int chance)
{
    if (chance < 0)
        chance = 0;

    if (chance > 99)
        chance = 99;

    you.last_be_hit_chance = chance;
    you.redraw_hit_chance = true;
}

void player_update_last_to_hit_chance(int chance)
{
    if (chance < 0)
        chance = 0;

    if (chance > 100)
        chance = 100;

    you.last_to_hit_chance = chance;
    you.redraw_hit_chance = true;
}

bool instant_resting = false;

void _heal_all_monsters()
{
    for (auto &mons : menv_real)
    {
        if (!mons.alive() || mons.wont_attack() || mons.cannot_fight())
            continue;
        if (mons_can_regenerate(&mons))
            mons.heal(mons.max_hit_points, true);
    }
}

void _instant_rest()
{
    if (instant_resting)
        return;

    instant_resting = true;
    if (player_regenerates_hp())
        inc_hp(get_hp_max() - get_hp());

    if (player_regenerates_sp())
        inc_sp(get_sp_max() - get_sp());

    if (player_regenerates_mp())
        inc_mp(get_mp_max() - get_mp());

    you.duration[DUR_BERSERK] = 0;
    you.duration[DUR_POISONING] = 0;
    calc_hp();

    player_after_each_turn();
    you.peace = 1000;

    dec_exhaust_player(1000);
    decrement_durations(5000, true);

    _heal_all_monsters();
    delete_all_clouds();

    instant_resting = false;
}

void _attempt_instant_rest_handle_no_visible_monsters()
{
    bool can_restore_all = true;

    vector<monster*> visible;
    visible.clear();

    // Find nearby monsters
    for (rectangle_iterator ri(you.pos(), 10); ri; ++ri)
    {
        if (monster *mon = monster_at(*ri))
        {
            if (mon->alive() && !mons_is_safe(mon, false, false, true))
            {
                visible.push_back(mon);
                break;
            }
        }
    }

    ldprf(LD_INSTAREST, "Checking for further out monsters...");

    if (visible.size() > 0)
    {
        ldprf(LD_INSTAREST, "Found further out monsters: %d (%s)", visible.size(), visible[0]->name(DESC_A, true).c_str());
        for(monster *near_monster : visible)
        {
            if (near_monster->behaviour == BEH_SLEEP)
                continue;

            coord_def to;
            int shortest = -1;
            for (edge_iterator ei(you.pos(), you.current_vision, true); ei; ++ei)
            {
                if (!near_monster->can_pass_through(*ei) || near_monster->cannot_move())
                    continue;

                if (!you.see_cell(*ei))
                    continue;

                monster_pathfind pf;
                if (!pf.init_pathfind(near_monster, *ei, true, false, true))
                    continue;

                if (shortest == -1 || pf.min_length < shortest)
                {
                    to = *ei;
                    shortest = pf.min_length;
                }
            }

            if (shortest != -1)
            {
                near_monster->move_to_pos(to);
                /*
                near_monster->speed_increment /= 2;
                behaviour_event(near_monster, ME_ALERT, &you, you.pos());
                 */
                ldprf(LD_INSTAREST, "Pulled in monster. recently_seen = %d.", you.monsters_recently_seen);
                can_restore_all = false;
            }
            else
            {
                ldprf(LD_INSTAREST, "Further out monster couldn't find there way here.");
            }

            if (coinflip())
                break;
        }
    }
    else
    {
        ldprf(LD_INSTAREST, "Didn't find any further out monsters");
    }

    if (can_restore_all && !poison_is_lethal())
    {
        _instant_rest();
    }
}

void attempt_instant_rest()
{
    const bool not_in_dangerous_branch = !player_in_hell() && you.where_are_you != BRANCH_ABYSS;
    const bool no_recent_monsters = you.monsters_recently_seen > 200 || you.monsters_recently_seen == 0;
    const bool safe = i_feel_safe();
    if (not_in_dangerous_branch && no_recent_monsters && safe)
    {
        you.monsters_recently_seen = 0;
        ldprf(LD_INSTAREST, "Recently_seen hit 0. Checking for nearby monsters...", you.monsters_recently_seen);

        vector<monster*> visible = get_nearby_monsters(false, false, true, false, false, false, -1);

        int reachable_count = 0;
        for (monster *m : visible)
        {
            monster_pathfind pf;
            if (!pf.init_pathfind(m, you.pos(), true, false, true))
                continue;

            reachable_count++;
        }

        if (reachable_count == 0)
        {
            ldprf(LD_INSTAREST, "No nearby monsters found.");
            _attempt_instant_rest_handle_no_visible_monsters();
        }
        else
        {
            ldprf(LD_INSTAREST, "Nearby monsters found: %d", reachable_count);
        }
    }
}

monster_type _pick_random_spirit()
{
    return random_choose_weighted(
        10, MONS_WRAITH,
        5, MONS_PHANTOM,
        5, MONS_HUNGRY_GHOST,
        5, MONS_SHADOW_WRAITH,
        5, MONS_FREEZING_WRAITH,
        5, MONS_PHANTASMAL_WARRIOR,
        5, MONS_GHOST,
        1, MONS_DROWNED_SOUL,
        1, MONS_EIDOLON,
        1, MONS_FLAYED_GHOST,
        1, MONS_FLYING_SKULL,
        1, MONS_INSUBSTANTIAL_WISP,
        1, MONS_LOST_SOUL,
        1, MONS_SILENT_SPECTRE,
        0);
}

void monster_died(mid_t mons_mid, bool was_hostile_and_seen, int mp_freeze, killer_type killer, int dead_monster_hd, bool left_corpse)
{
    if (crawl_state.sim_mode)
        return;

    bool can_rest = false;
    if (was_hostile_and_seen)
    {
        you.monsters_recently_seen--;
        ldprf(LD_INSTAREST, "Hostile seen monster died. recently_seen = %d", you.monsters_recently_seen);
        can_rest = true;
    }

    if (left_corpse && you.rune_curse_active[RUNE_CRYPT] && x_chance_in_y(1, 2))
    {
        const monster_type mon = _pick_random_spirit();

        mgen_data mg = mgen_data(mon, BEH_HOSTILE, &you, 3, SPELL_HAUNT, you.pos(), MHITNOT, MG_FORCE_BEH);
        mg.hd = dead_monster_hd;

        if (monster *mons = create_monster(mg, true))
        {
            can_rest = false;
            mpr("The creeping rune calls out to the dead to return.");

            mons->add_ench(mon_enchant(ENCH_HAUNTING, 1, &you, INFINITE_DURATION));
            mons->foe = MHITNOT;

            you.monsters_recently_seen++;
        }
    }

    if (mp_freeze)
        summoned_monster_died(mons_mid, mp_freeze, killer != KILL_RESET);

    if (can_rest)
        attempt_instant_rest();
}

void after_floor_change()
{
    crawl_state.free_stair_escape = true;
    you.monsters_recently_seen = 0;
    attempt_instant_rest();
}

