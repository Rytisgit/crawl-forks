/**
 * @file
 * @brief functions used during combat
 */

#include "AppHdr.h"

#include "fight.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "art-enum.h"
#include "coord.h"
#include "coordit.h"
#include "delay.h"
#include "english.h"
#include "env.h"
#include "fineff.h"
#include "fprop.h"
#include "god-passive.h" // passive_t::shadow_attacks
#include "hints.h"
#include "invent.h"
#include "item-prop.h"
#include "item-use.h"
#include "melee-attack.h"
#include "message.h"
#include "misc.h"
#include "mon-behv.h"
#include "mon-cast.h"
#include "mon-place.h"
#include "mon-util.h"
#include "ouch.h"
#include "player.h"
#include "prompt.h"
#include "random-var.h"
#include "religion.h"
#include "shopping.h"
#include "spl-miscast.h"
#include "spl-summoning.h"
#include "state.h"
#include "stringutil.h"
#include "target.h"
#include "terrain.h"
#include "transform.h"
#include "traps.h"
#include "travel.h"

/**
 * What are the odds of an HD-checking confusion effect (e.g. Confusing Touch,
 * Fungus Form, SPWPN_CHAOS maybe) to confuse a monster of the given HD?
 *
 * @param HD    The current hit dice (level) of the monster to confuse.
 * @return      A percentage chance (0-100) of confusing that monster.
 *              (Except it tops out at 80%.)
 */
int melee_confuse_chance(int HD)
{
    return max(80 * (24 - HD) / 24, 0);
}

// Quick wrapper for all the logic that follows a player attacking.
static bool _handle_player_attack(actor * defender, bool simu, int atk_num,
                                  int eff_atk_num, bool * did_hit,
                                  wu_jian_attack_type wu, int wu_num)
{
    melee_attack attk(&you, defender, atk_num, eff_atk_num);
    if (wu != WU_JIAN_ATTACK_NONE)
        attk.wu_jian_attack = wu;
    
    attk.wu_jian_number_of_targets = wu_num;

    if (simu)
        attk.simu = true;

    // Check if the player is fighting someone unsuitable
    if (!defender || !defender->alive())
    {
        if (eff_atk_num != 1)
            you.turn_is_over = false;
        return false;
    }

    // or with something unsuitable.
    if (you.can_see(*defender) && !simu
        && !wielded_weapons_check())
    {
        if (eff_atk_num != 1)
            you.turn_is_over = false;
        return false;
    }

    if (!attk.attack())
    {
        // Attack was cancelled or unsuccessful...
        if (attk.cancel_attack)
            you.turn_is_over = false;
        return !attk.cancel_attack;
    }

    if (did_hit)
        *did_hit = attk.did_hit;

    if (attk.cancel_remaining)
        return true;

    // A spectral weapon attacks whenever the player does
    if (!simu && you.props.exists("spectral_weapon") && (eff_atk_num == 0 || eff_atk_num == 2))
        trigger_spectral_weapon(&you, defender);

    // BCADNOTE: Should Dithmenos copy mount's attacks?
    if (!simu && will_have_passive(passive_t::shadow_attacks))
        dithmenos_shadow_melee(defender);

    return true;
}

static actor * _hydra_target(actor * original_target)
{
    if (original_target->alive()
        && adjacent(original_target->pos(), you.pos())
        && !original_target->is_banished()
        && !original_target->temp_attitude()) // If it's not hostile the melee attack charmed or pacified it.
    {
        return original_target;
    }

    int possible_targets = 0;
    actor * target = nullptr;

    for (adjacent_iterator ai(you.pos()); ai; ++ai)
    {
        actor * enemy = actor_at(*ai);

        if (enemy && !enemy->wont_attack())
        {
            possible_targets++;
            if (one_chance_in(possible_targets))
                target = enemy;
        }
    }

    return target;
}

static bool _handle_maws_attack(actor * target, bool simu, bool * did_hit, wu_jian_attack_type wu, int wu_num)
{
    const int lvl = you.get_mutation_level(MUT_JIBBERING_MAWS);

    if (!lvl)
        return false;

    const int atks = lvl + random2(lvl * 2);
    bool hit = false;

    for (int i = 1; i <= atks; ++i)
    {
        target = _hydra_target(target);

        if (!target)
            return hit;

        hit |= _handle_player_attack(target, simu, -1, i == atks ? 1 : 3, did_hit, wu, wu_num);
    }
    return hit;
}

// Hydra mount will keep attacking as long as there is a valid adjacent target (pseudocleave).
static bool _handle_hydra_attack(actor * target, bool simu, bool * did_hit, wu_jian_attack_type wu, int wu_num)
{
    if (you.mount != mount_type::hydra)
        return false;

    bool hit = false;

    for (int i = 1; i <= you.mount_heads; ++i)
    {
        target = _hydra_target(target);

        if (!target)
            return hit;

        hit |= _handle_player_attack(target, simu, 2, i == you.mount_heads ? 1 : 3, did_hit, wu, wu_num);

        if (!you.mounted()) // Spines killed your hydra.
            return hit;
    }
    return hit;
}

/**
 * Handle melee combat between attacker and defender.
 *
 * Works using the new fight rewrite. For a monster attacking, this method
 * loops through all their available attacks, instantiating a new melee_attack
 * for each attack. Combat effects should not go here, if at all possible. This
 * is merely a wrapper function which is used to start combat.
 *
 * @param[in] attacker,defender The (non-null) participants in the attack.
 *                              Either may be killed as a result of the attack.
 * @param[out] did_hit If non-null, receives true if the attack hit the
 *                     defender, and false otherwise.
 * @param simu Is this a simulated attack?  Disables a few problematic
 *             effects such as blood spatter and distortion teleports.
 *
 * @return Whether the attack took time (i.e. wasn't cancelled).
 */
bool fight_melee(actor *attacker, actor *defender, bool *did_hit,
    bool simu, wu_jian_attack_type wu, int wu_num)
{
    ASSERT(attacker); // XXX: change to actor &attacker
    ASSERT(defender); // XXX: change to actor &defender

    // A dead defender would result in us returning true without actually
    // taking an action.
    ASSERT(defender->alive());

    if (defender->is_player())
    {
        ASSERT(!crawl_state.game_is_arena());
        // Friendly and good neutral monsters won't attack unless confused.
        if (attacker->as_monster()->wont_attack()
            && !mons_is_confused(*attacker->as_monster())
            && !attacker->as_monster()->has_ench(ENCH_INSANE))
        {
            return false;
        }

        // In case the monster hasn't noticed you, bumping into it will
        // change that.
        behaviour_event(attacker->as_monster(), ME_ALERT, defender);
    }
    else if (attacker->is_player())
    {
        ASSERT(!crawl_state.game_is_arena());

        if (you.species == SP_FAIRY && !you.duration[DUR_CONFUSING_TOUCH])
        {
            mpr("You're too small and insubstantial to effectively melee attack.");
            stop_running();
            you.turn_is_over = false;
            return false;
        }

        // Can't damage orbs this way.
        if (mons_is_projectile(defender->type) && !you.confused())
        {
            you.turn_is_over = false;
            return false;
        }

        // We're trying to hit a monster, break out of travel/explore now.
        interrupt_activity(activity_interrupt::hit_monster, defender->as_monster());

        // BCADNOTE: The numbers involving auxilliary attacks are technically wrong for scorpion form
        // and will need to be changed should it be possible to get auxilliaries in scorpion form.
        bool local_time = false; // Not sure this is necessary; but I'm going to use it.
        bool attacked = false;
        coord_def pos = defender->pos();

        const bool skip_one = you.weapon(1) && you.weapon(1)->is_type(OBJ_STAVES, STAFF_LIFE);
        const bool skip_two = you.weapon(0) && you.weapon(0)->is_type(OBJ_STAVES, STAFF_LIFE);

        bool xtra_atk = (you.form == transformation::scorpion || you.get_mutation_level(MUT_JIBBERING_MAWS));
        bool mount_atk = false;

        if (you.mounted() && !you.petrified(true))
        {
            if (you.duration[DUR_MOUNT_SLOW] || you.petrifying(true))
            {
                if (you.mount_energy >= 15)
                {
                    mount_atk = true;
                    you.mount_energy -= 15;
                }
            }
            else if (you.mount_energy >= 10)
            {
                mount_atk = true;
                you.mount_energy -= 10;
            }
        }
        
        xtra_atk |= mount_atk;

        if ((!you.weapon(0) || is_melee_weapon(*you.weapon(0))) && !skip_one)
        {
            attacked = true;
            if ((!you.weapon(1) || is_melee_weapon(*you.weapon(1))) && !you.get_mutation_level(MUT_MISSING_HAND))
            {
                if (you.weapon(0) && you.hands_reqd(*you.weapon(0)) == HANDS_TWO)
                    local_time = _handle_player_attack(defender, simu, 0, xtra_atk ? 0 : 2, did_hit, wu, wu_num);
                else if (you.weapon(1) && you.hands_reqd(*you.weapon(1)) == HANDS_TWO)
                    local_time = _handle_player_attack(defender, simu, 1, xtra_atk ? 0 : 2, did_hit, wu, wu_num);
                else
                {
                    local_time = _handle_player_attack(defender, simu, 0, 0, did_hit, wu, wu_num);
                    if (!defender->alive()
                        || defender->pos() != pos
                        || defender->is_banished()
                        || defender->temp_attitude() // If it's not hostile the melee attack charmed or pacified it.
                        || skip_two) 
                    {
                        local_time |= _handle_maws_attack(defender, simu, did_hit, wu, wu_num);
                        local_time |= _handle_hydra_attack(defender, simu, did_hit, wu, wu_num);
                        return local_time;
                    }
                    else
                        local_time |= _handle_player_attack(defender, simu, 1, xtra_atk ? 3 : 1, did_hit, wu, wu_num);
                }
            }
            else
                local_time = _handle_player_attack(defender, simu, 0, xtra_atk ? 0 : 2, did_hit, wu, wu_num);
        }
        else if (!you.weapon(1) || is_melee_weapon(*you.weapon(1)))
        {
            if (!(you.weapon(0) && you.hands_reqd(*you.weapon(0)) == HANDS_TWO) && !you.get_mutation_level(MUT_MISSING_HAND))
            {
                attacked = true;
                local_time = _handle_player_attack(defender, simu, 1, xtra_atk ? 0 : 2, did_hit, wu, wu_num);
            }
        }

        if (!attacked)
        {
            mpr("You can't melee attack with what you're currently wielding.");

            you.turn_is_over = false;
            return false;
        }
        else
        {
            if (!defender->alive()
                || defender->pos() != pos
                || defender->is_banished()
                || defender->temp_attitude()) // If it's not hostile the melee attack charmed or pacified it.
            {
                local_time |= _handle_maws_attack(defender, simu, did_hit, wu, wu_num);
                local_time |= _handle_hydra_attack(defender, simu, did_hit, wu, wu_num);
                return local_time;
            }

            if (you.form == transformation::scorpion)
            {
                local_time |= _handle_player_attack(defender, simu, 2, 3, did_hit, wu, wu_num);

                if (!defender->alive()
                    || defender->pos() != pos
                    || defender->is_banished()
                    || defender->temp_attitude()) // If it's not hostile the melee attack charmed or pacified it.
                {
                    return local_time;
                }

                local_time |= _handle_player_attack(defender, simu, 3, 1, did_hit, wu, wu_num);
            }
            else if (xtra_atk)
            {
                if (you.get_mutation_level(MUT_JIBBERING_MAWS))
                    local_time |= _handle_maws_attack(defender, simu, did_hit, wu, wu_num);

                if (you.mount == mount_type::hydra)
                    local_time |= _handle_hydra_attack(defender, simu, did_hit, wu, wu_num);
                else if (mount_atk)
                    local_time |= _handle_player_attack(defender, simu, 2, 1, did_hit, wu, wu_num);
            }
        }

        return local_time;
    }

    // If execution gets here, attacker != Player, so we can safely continue
    // with processing the number of attacks a monster has without worrying
    // about unpredictable or weird results from players.

    // If this is a spectral weapon check if it can attack
    if (attacker->type == MONS_SPECTRAL_WEAPON
        && !confirm_attack_spectral_weapon(attacker->as_monster(), defender))
    {
        // Pretend an attack happened,
        // so the weapon doesn't advance unecessarily.
        return true;
    }

    const int nrounds = MAX_NUM_ATTACKS;
    coord_def pos = defender->pos();

    // Melee combat, tell attacker to wield its melee weapon.
    attacker->as_monster()->wield_melee_weapon();

    int effective_attack_number = 0;
    int attack_number;
    bool multiattacking = false;
    int repeats = attacker->heads();
    int held_attack_num = 0;
    for (attack_number = 0; attack_number < nrounds && attacker->alive();
         ++attack_number, ++effective_attack_number)
    {
        if (!attacker->alive())
            return false;

        if (attacker->is_monster() && attacker->as_monster()->has_hydra_multi_attack(attack_number))
        {
            multiattacking = true;
            held_attack_num = attack_number;
        }

        if (multiattacking)
        {
            repeats--;
            if (repeats <= 0)
                multiattacking = false;
            attack_number = held_attack_num;
        }

        // Monster went away?
        if (!defender->alive()
            || defender->pos() != pos
            || defender->is_banished())
        {
            if (attacker == defender
               || !attacker->as_monster()->has_multitargeting())
            {
                break;
            }

            // Hydras can try and pick up a new monster to attack to
            // finish out their round. -cao
            bool end = true;
            for (adjacent_iterator i(attacker->pos()); i; ++i)
            {
                if (*i == you.pos()
                    && !mons_aligned(attacker, &you))
                {
                    attacker->as_monster()->foe = MHITYOU;
                    attacker->as_monster()->target = you.pos();
                    defender = &you;
                    end = false;
                    break;
                }

                monster* mons = monster_at(*i);
                if (mons && !mons_aligned(attacker, mons))
                {
                    defender = mons;
                    end = false;
                    pos = mons->pos();
                    break;
                }
            }

            // No adjacent hostiles.
            if (end)
                break;
        }

        if (!simu && attacker->is_monster()
            && mons_attack_spec(*attacker->as_monster(), attack_number, true)
                   .flavour == AF_KITE
            && attacker->as_monster()->foe_distance() == 1
            && attacker->reach_range() == REACH_TWO
            && x_chance_in_y(3, 5))
        {
            monster* mons = attacker->as_monster();
            coord_def foepos = mons->get_foe()->pos();
            coord_def hopspot = mons->pos() - (foepos - mons->pos()).sgn();

            bool found = false;
            if (!monster_habitable_grid(mons, grd(hopspot)) ||
                actor_at(hopspot))
            {
                for (adjacent_iterator ai(mons->pos()); ai; ++ai)
                {
                    if (ai->distance_from(foepos) != 2)
                        continue;
                    else
                    {
                        if (monster_habitable_grid(mons, grd(*ai))
                            && !actor_at(*ai))
                        {
                            hopspot = *ai;
                            found = true;
                            break;
                        }
                    }
                }
            }
            else
                found = true;

            if (found)
            {
                const bool could_see = you.can_see(*mons);
                if (mons->move_to_pos(hopspot))
                {
                    if (could_see || you.can_see(*mons))
                    {
                        mprf("%s hops backward while attacking.",
                             mons->name(DESC_THE, true).c_str());
                    }
                    mons->speed_increment -= 2; // Add a small extra delay
                }
            }
        }

        melee_attack melee_attk(attacker, defender, attack_number,
                                effective_attack_number);

        if (simu)
            melee_attk.simu = true;

        // If the attack fails out, keep effective_attack_number up to
        // date so that we don't cause excess energy loss in monsters
        if (!melee_attk.attack())
            effective_attack_number = melee_attk.effective_attack_number;
        else if (did_hit && !(*did_hit))
            *did_hit = melee_attk.did_hit;

        fire_final_effects();
    }

    // A spectral weapon attacks whenever the player does
    if (!simu && attacker->props.exists("spectral_weapon"))
        trigger_spectral_weapon(attacker, defender);

    return true;
}

/**
 * If the given attacker attacks the given defender right now, what kind of
 * extra-damage "stab" attack can the attacker perform, if any?
 *
 * @param attacker  The attacker; may be null.
 * @param defender  The defender.
 * @param actual    True if we're actually committing to a stab, false if we're
 *                  just checking for display purposes.
 * @return          The best (most damaging) kind of stab available to the
 *                  attacker against this defender, or STAB_NO_STAB.
 */
stab_type find_stab_type(const actor *attacker,
                         const actor &defender,
                         bool actual)
{
    const monster* def = defender.as_monster();

    // Stabbing intelligent monsters is unchivalric, and disabled under TSO!
    // When just checking for display purposes, still indicate when monsters
    // are sleeping/paralysed etc.
    if (actual && attacker && attacker->is_player()
        && def && have_passive(passive_t::no_stabbing))
    {
        return STAB_NO_STAB;
    }

    // No stabbing monsters that cannot fight (e.g. plants) or monsters
    // the attacker can't see (either due to invisibility or being behind
    // opaque clouds).
    if (def && mons_is_firewood(*def))
        return STAB_NO_STAB;

    if (attacker && !attacker->can_see(defender))
        return STAB_NO_STAB;

    // sleeping
    if (defender.asleep())
        return STAB_SLEEPING;

    // paralysed
    if (defender.paralysed())
        return STAB_PARALYSED;

    // petrified
    if (defender.petrified())
        return STAB_PETRIFIED;

    // petrifying
    if (def && def->petrifying())
        return STAB_PETRIFYING;

    // held in a net
    if (def && def->caught())
        return STAB_HELD_IN_NET;

    // invisible
    if (attacker && !attacker->visible_to(&defender))
        return STAB_INVISIBLE;

    // fleeing
    if (def && mons_is_fleeing(*def))
        return STAB_FLEEING;

    // allies
    if (def && def->friendly())
        return STAB_ALLY;

    // confused (but not perma-confused)
    if (def && mons_is_confused(*def, false))
        return STAB_CONFUSED;

    // Distracted (but not batty); this only applies to players.
    if (attacker && attacker->is_player()
        && def && def->foe != MHITYOU && !mons_is_batty(*def))
    {
        return STAB_DISTRACTED;
    }

    return STAB_NO_STAB;
}

/**
 * What bonus does this type of stab give the player when attacking?
 *
 * @param   The type of stab in question; e.g. STAB_SLEEPING.
 * @return  The bonus the stab gives. Note that this is used as a divisor for
 *          damage, so the larger the value we return here, the less bonus
 *          damage will be done.
 */
int stab_bonus_denom(stab_type stab)
{
    // XXX: if we don't get rid of this logic, turn it into a static array.
    switch (stab)
    {
        case STAB_NO_STAB:
        case NUM_STABS:
            return 0;
        case STAB_SLEEPING:
        case STAB_PARALYSED:
        case STAB_PETRIFIED:
            return 1;
        default:
            return 4;
    }
}

static bool is_boolean_resist(beam_type flavour)
{
    switch (flavour)
    {
    case BEAM_ELECTRICITY:
    case BEAM_MIASMA: // rotting
    case BEAM_STICKY_FLAME:
    case BEAM_WATER:  // water asphyxiation damage,
                      // bypassed by being water inhabitant.
    case BEAM_POISON:
    case BEAM_POISON_ARROW:
        return true;
    default:
        return false;
    }
}

// Gets the percentage of the total damage of this damage flavour that can
// be resisted.
static inline int get_resistible_fraction(beam_type flavour)
{
    switch (flavour)
    {
    case BEAM_CRYSTAL_SPEAR:
    case BEAM_CRYSTAL_FIRE:
    case BEAM_CRYSTAL_ICE:
        return 20;

    // Drowning damage from water is resistible by being a water thing, or
    // otherwise asphyx resistant.
    case BEAM_WATER:
    case BEAM_ACID_WAVE:
        return 40;

    // Assume ice storm and throw icicle are mostly solid.
    case BEAM_FREEZE:
    case BEAM_ICE:
        return 40;

    case BEAM_ICY_DEVASTATION:
        return 30;

    case BEAM_LAVA:
        return 55;

    case BEAM_POISON_ARROW:
        return 30;

    case BEAM_POISON:
        return 50;

    default:
        return 100;
    }
}

static int _beam_to_resist(const actor* defender, beam_type flavour, bool mount)
{
    switch (flavour)
    {
        case BEAM_SLASH:
            return defender->res_slash(mount);
        case BEAM_BLUDGEON:
            return defender->res_bludgeon(mount);
        case BEAM_PIERCE:
            return defender->res_pierce(mount);
        case BEAM_CRYSTAL_FIRE:
        case BEAM_FIRE:
        case BEAM_LAVA:
            return defender->res_fire(mount);
        case BEAM_DAMNATION:
            return defender->res_hellfire(mount);
        case BEAM_STEAM:
            return defender->res_steam(mount);
        case BEAM_COLD:
        case BEAM_ICY_DEVASTATION:
        case BEAM_FREEZE:
        case BEAM_ICE:
        case BEAM_CRYSTAL_ICE:
            return defender->res_cold(mount);
        case BEAM_WATER:
            return defender->res_water_drowning(mount);
        case BEAM_ELECTRICITY:
            return defender->res_elec(mount);
        case BEAM_NEG:
        case BEAM_PAIN:
        case BEAM_MALIGN_OFFERING:
            return defender->res_negative_energy(mount);
        case BEAM_ACID:
            return defender->res_acid(mount);
        case BEAM_POISON:
        case BEAM_POISON_ARROW:
            return defender->res_poison(mount);
        case BEAM_HOLY:
            return defender->res_holy_energy(mount);
        default:
            return 0;
    }
}

static bool _is_physical_resist(beam_type flavour)
{
    switch (flavour)
    {
    case BEAM_BLUDGEON:
    case BEAM_SLASH:
    case BEAM_PIERCE:
        return true;
    default:
        return false;
    }
}

static bool _dragonskin_affected (beam_type flavour)
{
    switch (flavour)
    {
    case BEAM_FIRE:
    case BEAM_LAVA:
    case BEAM_STEAM:
    case BEAM_COLD:
    case BEAM_ICY_DEVASTATION:
    case BEAM_FREEZE:
    case BEAM_ICE:
    case BEAM_ELECTRICITY:
    case BEAM_NEG:
    case BEAM_PAIN:
    case BEAM_MALIGN_OFFERING:
    case BEAM_ACID:
    case BEAM_POISON:
    case BEAM_POISON_ARROW:
    case BEAM_CRYSTAL_FIRE:
    case BEAM_CRYSTAL_ICE:
        return true;
    default:
        return false;
    }
}

/**
 * Adjusts damage for elemental resists, electricity and poison.
 *
 * For players, damage is reduced to 1/2, 1/3, or 1/5 if res has values 1, 2,
 * or 3, respectively. "Boolean" resists (rElec, rPois) reduce damage to 1/3.
 * rN is a special case that reduces damage to 1/2, 1/4, 0 instead.
 *
 * For monsters, damage is reduced to 1/2, 1/5, and 0 for 1/2/3 resistance.
 * "Boolean" resists give 1/3, 1/6, 0 instead.
 *
 * @param defender      The victim of the attack.
 * @param flavour       The type of attack having its damage adjusted.
 *                      (Does not necessarily imply the attack is a beam.)
 * @param rawdamage     The base damage, to be adjusted by resistance.
 * @param mount         Checking the mount's resistance instead of the players (defender is a player).
 * @return              The amount of damage done, after resists are applied.
 */
int resist_adjust_damage(const actor* defender, beam_type flavour, int rawdamage, bool mount)
{
    if (defender->is_fairy() && !mount)
        return rawdamage;

    int res = _beam_to_resist(defender, flavour, mount);
    bool is_mon = defender->is_monster();

    // This special case is like 90% for Tiamat; unless a different draconian picks up 
    // her cloak since most monsters don't use armour for non-body slot; but it's good
    // to make the unique more unique and future-proofing should we start handing out
    // cloaks more often.
    if (is_mon)
    {
        item_def * cloak = defender->as_monster()->mslot_item(MSLOT_ARMOUR);

        if (cloak && is_unrandom_artefact(*cloak, UNRAND_DRAGONSKIN)
            && coinflip() && _dragonskin_affected(flavour))
        {
            res++;
        }

        item_def * ring = defender->as_monster()->mslot_item(MSLOT_JEWELLERY);

        if (ring && ring->is_type(OBJ_JEWELLERY, AMU_CHAOS)
            && _dragonskin_affected(flavour))
        {
            int x = random2(6);

            switch (x)
            {
            case 0:
            case 1:
            case 2:
                res++;
                break;
            case 3:
                res--;
                break;
            default:
                break;
            }
        }
    }

    is_mon |= mount; // Mounts act like monsters in this way.

    if (!res)
        return rawdamage;

    const int resistible_fraction = get_resistible_fraction(flavour);

    int resistible = rawdamage * resistible_fraction / 100;
    const int irresistible = rawdamage - resistible;

    if (res > 0)
    {
        const bool immune_at_3_res = is_mon
                                     || flavour == BEAM_NEG
                                     || flavour == BEAM_PAIN
                                     || flavour == BEAM_MALIGN_OFFERING
                                     || flavour == BEAM_HOLY
                                     || flavour == BEAM_POISON
                                     || flavour == BEAM_ACID
                                     // just the resistible part
                                     || flavour == BEAM_POISON_ARROW;

        if (immune_at_3_res && res >= 3 || res > 3)
            resistible = 0;
        else
        {
            // Is this a resist that claims to be boolean for damage purposes?
            const int bonus_res = (is_boolean_resist(flavour) ? 1 : 0);

            // Monster resistances are stronger than player versions.
            if (is_mon && !_is_physical_resist(flavour))
                resistible /= 1 + bonus_res + res * res;
            else if (flavour == BEAM_NEG
                     || flavour == BEAM_PAIN
                     || flavour == BEAM_MALIGN_OFFERING)
            {
                resistible /= res * 2;
            }
            else
                resistible /= (3 * res + 1) / 2 + bonus_res;
        }
    }
    else if (res < 0)
        resistible = resistible * 15 / 10;

    return max(resistible + irresistible, 0);
}

// Reduce damage by AC.
// In most cases, we want AC to mostly stop weak attacks completely but affect
// strong ones less, but the regular formula is too hard to apply well to cases
// when damage is spread into many small chunks.
//
// Every point of damage is processed independently. Every point of AC has
// an independent 1/81 chance of blocking that damage.
//
// AC 20 stops 22% of damage, AC 40 -- 39%, AC 80 -- 63%.
int apply_chunked_AC(int dam, int ac)
{
    double chance = pow(80.0/81, ac);
    uint64_t cr = chance * (((uint64_t)1) << 32);

    int hurt = 0;
    for (int i = 0; i < dam; i++)
        if (rng::get_uint32() < cr)
            hurt++;

    return hurt;
}

///////////////////////////////////////////////////////////////////////////

bool wielded_weapons_check()
{
    const item_def * weap0 = you.weapon(0);
    const item_def * weap1 = you.weapon(1);
    bool warn0 = true;
    bool warn1 = true;
    bool unarmed_warning = false;
    bool penance = false;

    if (you.received_weapon_warning || you.confused())
        return true;

    if (weap0 && !is_melee_weapon(*weap0))
        weap0 = nullptr;

    if (weap1 && !is_melee_weapon(*weap1))
        weap1 = nullptr;

    if (!weap0 && !weap1 && (you.skill(SK_UNARMED_COMBAT) < 3))
        unarmed_warning = true;

    // Don't pester the player if they don't have any melee weapons yet.
    if (!any_of(you.inv.begin(), you.inv.end(),
                       [](item_def &it)
                       { return is_melee_weapon(it) && can_wield(&it); }))
    {
        return true;
    }

    if (weap0 && !needs_handle_warning(*weap0, OPER_ATTACK, penance))
        warn0 = false;

    if (weap1 && !needs_handle_warning(*weap1, OPER_ATTACK, penance))
        warn1 = false;

    if (!weap0)
        warn0 = false;

    if (!weap1)
        warn1 = false;

    if (!warn0 && !warn1)
        return true;

    string prompt;
    if (unarmed_warning)
        prompt = "Really attack unarmed?";
    else
    {
        prompt = make_stringf("Really attack while wielding %s%s%s?",
            warn0 ? weap0->name(DESC_YOUR).c_str() : "",
            warn0 && warn1 ? " and " : "",
            warn1 ? weap1->name(DESC_YOUR).c_str() : "");
    }
    if (penance)
        prompt += " This could place you under penance!";

    const bool result = yesno(prompt.c_str(), true, 'n');

    if (!result)
        canned_msg(MSG_OK);

    learned_something_new(HINT_WIELD_WEAPON); // for hints mode Rangers

    // Don't warn again if you decide to continue your attack.
    if (result)
        you.received_weapon_warning = true;

    return result;
}

/**
 * Should the given attacker cleave into the given victim with an axe or axe-
 * like weapon?
 *
 * @param attacker  The creature doing the cleaving.
 * @param defender  The potential cleave-ee.
 * @return          True if the defender is an enemy of the defender; false
 *                  otherwise.
 */
static bool _dont_harm(const actor &attacker, const actor &defender)
{
    if (mons_aligned(&attacker, &defender))
        return true;

    if (defender.is_player())
        return attacker.wont_attack();

    if (attacker.is_player())
    {
        return defender.wont_attack()
               || mons_attitude(*defender.as_monster()) == ATT_NEUTRAL;
    }

    return false;
}

/**
 * List potential cleave targets (adjacent hostile creatures), including the
 * defender itself.
 *
 * @param attacker[in]   The attacking creature.
 * @param def[in]        The location of the targeted defender.
 * @param targets[out]   A list to be populated with targets.
 * @param which_attack   The attack_number (default -1, which uses the default weapon).
 */
void get_cleave_targets(const actor &attacker, const coord_def& def,
                        list<actor*> &targets, int which_attack)
{
    // Prevent scanning invalid coordinates if the attacker dies partway through
    // a cleave (due to hitting explosive creatures, or perhaps other things)
    if (!attacker.alive())
        return;

    if (actor_at(def))
        targets.push_back(actor_at(def));

    const item_def* weap = attacker.weapon(which_attack);

    if (weap && weap->is_type(OBJ_WEAPONS, WPN_SCYTHE))
    {
        for (rectangle_iterator ri(attacker.pos(), 2); ri; ++ri)
        {
            actor *target = actor_at(*ri);
            if (target && attacker.see_cell_no_trans(*ri) &&
                    !_dont_harm(attacker, *target) && (*ri != def))
                targets.push_back(target);
        }
    }

    else if (weap && (item_attack_skill(*weap) == SK_AXES_HAMMERS || weap->is_type(OBJ_WEAPONS, WPN_CLEAVER))
            || attacker.is_player()
               && (you.form == transformation::hydra && you.heads() > 1
                   || you.duration[DUR_CLEAVE]))
    {
        const coord_def atk = attacker.pos();
        coord_def atk_vector = def - atk;
        const int dir = random_choose(-1, 1);

        for (int i = 0; i < 7; ++i)
        {
            atk_vector = rotate_adjacent(atk_vector, dir);

            actor *target = actor_at(atk + atk_vector);
            if (target && !_dont_harm(attacker, *target))
                targets.push_back(target);
        }
    }
}

/**
 * Attack a provided list of cleave targets.
 *
 * @param attacker                  The attacking creature.
 * @param targets                   The targets to cleave.
 * @param attack_number             ?
 * @param effective_attack_number   ?
 */
void attack_cleave_targets(actor &attacker, list<actor*> &targets,
                           int attack_number, int effective_attack_number,
                           wu_jian_attack_type wu_jian_attack)
{
    if (!attacker.alive())
        return;

    if (attacker.is_player())
    {
        if ((wu_jian_attack == WU_JIAN_ATTACK_WHIRLWIND
             || wu_jian_attack == WU_JIAN_ATTACK_WALL_JUMP
             || wu_jian_attack == WU_JIAN_ATTACK_TRIGGERED_AUX))
        {
            return; 
        }
    }

    item_def* wpn = attacker.weapon(attack_number);
    int range = 1;
    if (wpn && wpn->base_type == OBJ_WEAPONS && wpn->sub_type == WPN_SCYTHE)
        range = 2;

    while (attacker.alive() && !targets.empty())
    {
        actor* def = targets.front();

        if (cell_is_solid(def->pos()) && def->is_monster() && mons_wall_shielded(*def->as_monster()))
            mprf("%s clang%s %s on the wall.", attacker.is_player() ? "You" : attacker.name(DESC_THE).c_str(), 
                 attacker.is_player() ? "" : "s",wpn->name(DESC_A).c_str());
        else if (def && def->alive() && !_dont_harm(attacker, *def) && 
                (grid_distance(attacker.pos(), def->pos()) <= range))
        {
            melee_attack attck(&attacker, def, attack_number,
                               ++effective_attack_number, true);

            attck.wu_jian_attack = wu_jian_attack;
            attck.attack();
        }
        targets.pop_front();
    }
}

/**
 * What skill is required to reach mindelay with a weapon? May be >27.
 * @param weapon The weapon to be considered.
 * @returns The level of the relevant skill you must reach.
 */
int weapon_min_delay_skill(const item_def &weapon)
{
    int speed = 1;
    if (weapon.base_type == OBJ_SHIELDS)
        speed = property(weapon, PSHD_SPEED);
    else
        speed = property(weapon, PWPN_SPEED);
    const int mindelay = weapon_min_delay(weapon, false);
    return (speed - mindelay) * 2;
}

/**
 * What's the base delay at 0 skill with the given weapon combination?
 * @param weap0 The first weapon.
 * @param weap1 The second weapon.
 * @returns base delay when wielding those two weapons.
 */

int dual_wield_base_delay(const item_def &weap0, const item_def &weap1)
{
    const int wpn0_delay = weapon_delay(weap0);
    const int wpn1_delay = weapon_delay(weap1);
    const int wpn0_mindelay = weapon_min_delay(weap0);
    const int wpn1_mindelay = weapon_min_delay(weap1);
    const int delay_div0 = wpn0_delay - wpn0_mindelay;
    const int delay_div1 = wpn1_delay - wpn1_mindelay;

    bool mismatch = (item_attack_skill(weap0) != item_attack_skill(weap1));

    if (mismatch)
        return max(wpn0_mindelay, wpn1_mindelay) + 3 * (delay_div0 + delay_div1) / 2;

    if (delay_div0 > delay_div1)
        return max(wpn0_mindelay, wpn1_mindelay) + delay_div0 + delay_div1 / 2;
    return max(wpn0_mindelay, wpn1_mindelay) + delay_div1 + delay_div0 / 2;
}

/**
* How much skill is necessary to hit mindelay with the given weapon combo?
* Can be >27.
* Only works for matching skill types; different skill types use a simpler formula.
* @param weap0 The first weapon.
* @param weap1 The second weapon.
* @returns base delay when wielding those two weapons.
*/

int dual_wield_mindelay_skill(const item_def &weap0, const item_def &weap1)
{
    const int slower_skill = max(weapon_min_delay_skill(weap0), weapon_min_delay_skill(weap1));
    const int faster_skill = min(weapon_min_delay_skill(weap0), weapon_min_delay_skill(weap1));

    return slower_skill + (faster_skill) / 2;
}

/**
 * How fast will this weapon get from your skill training?
 *
 * @param weapon the weapon to be considered.
 * @param check_speed whether to take it into account if the weapon has the
 *                    speed brand.
 * @return How many aut the fastest possible attack with this weapon would take.
 */
int weapon_min_delay(const item_def &weapon, bool check_speed)
{
    int base = 20;
    if (weapon.base_type == OBJ_SHIELDS)
    {
        if (!is_hybrid(weapon.sub_type))
            return 5;
        base = property(weapon, PSHD_SPEED);
    }
    else base = property(weapon, PWPN_SPEED);
    int min_delay = base/2;

    // Hammers are special cased slightly.
    if (weapon.is_type(OBJ_WEAPONS, WPN_HAMMER))
        min_delay = 6;

    if (is_unrandom_artefact(weapon, UNRAND_ZEPHYR))
        min_delay = 3;

    // Short blades and bows can get up to at least unarmed speed.
    if (item_attack_skill(weapon) == SK_SHORT_BLADES 
        || item_attack_skill(weapon) == SK_BOWS)
        min_delay = min(5, min_delay);

    // All weapons have min delay 7 or better
    if (min_delay > 7)
        min_delay = 7;

    // ...except crossbows...
    if (item_attack_skill(weapon) == SK_CROSSBOWS && min_delay < 10)
        min_delay = 10;

    // ... and unless it would take more than skill 27 to get there.
    // Round up the reduction from skill, so that min delay is rounded down.
    min_delay = max(min_delay, base - (MAX_SKILL_LEVEL + 1)/2);

    if (check_speed && get_weapon_brand(weapon) == SPWPN_SPEED)
    {
        min_delay *= 2;
        min_delay /= 3;
    }

    // never go faster than speed 3 (ie 3.33 attacks per round)
    if (min_delay < 3)
        min_delay = 3;

    return min_delay;
}

int mons_weapon_damage_rating(const item_def &launcher)
{
    return property(launcher, PWPN_DAMAGE) + launcher.plus;
}

// Returns a rough estimate of damage from firing/throwing missile.
int mons_missile_damage(monster* mons, const item_def *launch,
                        const item_def *missile)
{
    if (!missile || (!launch && !is_throwable(mons, *missile)))
        return 0;

    const int missile_damage = property(*missile, PWPN_DAMAGE) / 2 + 1;
    const int launch_damage  = launch? property(*launch, PWPN_DAMAGE) : 0;
    return max(0, launch_damage + missile_damage);
}

bool bad_attack(const monster *mon, string& adj, string& suffix,
                bool& would_cause_penance, coord_def attack_pos)
{
    ASSERT(mon); // XXX: change to const monster &mon
    ASSERT(!crawl_state.game_is_arena());

    if (!you.can_see(*mon))
        return false;

    if (you.weapon(0) && you.weapon(0)->is_type(OBJ_STAVES, STAFF_LIFE))
        return false;

    if (you.weapon(1) && you.weapon(1)->is_type(OBJ_STAVES, STAFF_LIFE))
        return false;

    if (attack_pos == coord_def(0, 0))
        attack_pos = you.pos();

    adj.clear();
    suffix.clear();
    would_cause_penance = false;

    if (is_sanctuary(mon->pos()) || is_sanctuary(attack_pos))
        suffix = ", despite your sanctuary";

    if (you.duration[DUR_LIFESAVING]
        && mon->holiness() & (MH_NATURAL | MH_PLANT))
    {
        suffix = " while asking for your life to be spared";
        would_cause_penance = true;
    }

    if (you_worship(GOD_JIYVA) && mons_is_slime(*mon)
        && !(mon->is_shapeshifter() && (mon->flags & MF_KNOWN_SHIFTER)))
    {
        would_cause_penance = true;
        return true;
    }

    if (mon->friendly())
    {
        if (god_hates_attacking_friend(you.religion, *mon))
        {
            adj = "your ally ";

            monster_info mi(mon, MILEV_NAME);
            if (!mi.is(MB_NAME_UNQUALIFIED))
                adj += "the ";

            would_cause_penance = true;

        }
        else
        {
            adj = "your ";

            monster_info mi(mon, MILEV_NAME);
            if (mi.is(MB_NAME_UNQUALIFIED))
                adj += "ally ";
        }

        return true;
    }

    if (mon->neutral() && is_good_god(you.religion))
    {
        adj += "neutral ";
        if (you_worship(GOD_SHINING_ONE) || you_worship(GOD_ELYVILON))
            would_cause_penance = true;
    }
    else if (mon->wont_attack())
    {
        adj += "non-hostile ";
        if (you_worship(GOD_SHINING_ONE) || you_worship(GOD_ELYVILON))
            would_cause_penance = true;
    }

    return !adj.empty() || !suffix.empty();
}

bool stop_attack_prompt(const monster* mon, bool beam_attack,
                        coord_def beam_target, bool *prompted,
                        coord_def attack_pos)
{
    ASSERT(mon); // XXX: change to const monster &mon
    bool penance = false;

    if (prompted)
        *prompted = false;

    if (crawl_state.disables[DIS_CONFIRMATIONS])
        return false;

    if (you.confused() || !you.can_see(*mon))
        return false;

    string adj, suffix;
    if (!bad_attack(mon, adj, suffix, penance, attack_pos))
        return false;

    // Listed in the form: "your rat", "Blork the orc".
    string mon_name = mon->name(DESC_PLAIN);
    if (starts_with(mon_name, "the ")) // no "your the Royal Jelly" nor "the the RJ"
        mon_name = mon_name.substr(4); // strlen("the ")
    if (!starts_with(adj, "your"))
        adj = "the " + adj;
    mon_name = adj + mon_name;
    string verb;
    if (beam_attack)
    {
        verb = "fire ";
        if (beam_target == mon->pos())
            verb += "at ";
        else
        {
            verb += "in " + apostrophise(mon_name) + " direction";
            mon_name = "";
        }
    }
    else
        verb = "attack ";

    const string prompt = make_stringf("Really %s%s%s?%s",
             verb.c_str(), mon_name.c_str(), suffix.c_str(),
             penance ? " This attack would place you under penance!" : "");

    if (prompted)
        *prompted = true;

    if (yesno(prompt.c_str(), false, 'n'))
        return false;
    else
    {
        canned_msg(MSG_OK);
        return true;
    }
}

bool stop_attack_prompt(targeter &hitfunc, const char* verb,
                        function<bool(const actor *victim)> affects,
                        bool *prompted, const monster *defender)
{
    if (crawl_state.disables[DIS_CONFIRMATIONS])
        return false;

    if (crawl_state.which_god_acting() == GOD_XOM)
        return false;

    if (you.confused())
        return false;

    string adj, suffix;
    bool penance = false;
    bool defender_ok = true;
    counted_monster_list victims;
    for (distance_iterator di(hitfunc.origin, false, true, LOS_RADIUS); di; ++di)
    {
        if (hitfunc.is_affected(*di) <= AFF_NO)
            continue;

        const monster* mon = monster_at(*di);
        if (!mon || !you.can_see(*mon))
            continue;

        if (affects && !affects(mon))
            continue;

        string adjn, suffixn;
        bool penancen = false;
        if (bad_attack(mon, adjn, suffixn, penancen))
        {
            // record the adjectives for the first listed, or
            // first that would cause penance
            if (victims.empty() || penancen && !penance)
                adj = adjn, suffix = suffixn, penance = penancen;

            victims.add(mon);

            if (defender && defender == mon)
                defender_ok = false;
        }
    }

    if (victims.empty())
        return false;

    // Listed in the form: "your rat", "Blork the orc".
    string mon_name = victims.describe(DESC_PLAIN);
    if (starts_with(mon_name, "the ")) // no "your the Royal Jelly" nor "the the RJ"
        mon_name = mon_name.substr(4); // strlen("the ")
    if (!starts_with(adj, "your"))
        adj = "the " + adj;
    mon_name = adj + mon_name;

    const string prompt = make_stringf("Really %s%s %s%s?%s",
             verb, defender_ok ? " near" : "", mon_name.c_str(),
             suffix.c_str(),
             penance ? " This attack would place you under penance!" : "");

    if (prompted)
        *prompted = true;

    if (yesno(prompt.c_str(), false, 'n'))
        return false;
    else
    {
        canned_msg(MSG_OK);
        return true;
    }
}

/**
 * Does the player have Olgreb's Toxic Radiance up that would/could cause
 * a hostile summon to be created? If so, prompt the player as to whether they
 * want to continue to create their summon. Note that this prompt is never a
 * penance prompt, because we don't cause penance when monsters enter line of
 * sight when OTR is active, regardless of how they entered LOS.
 *
 * @param verb    The verb to be used in the prompt. Defaults to "summon".
 * @return        True if the player wants to abort.
 */
bool otr_stop_summoning_prompt(string verb)
{
    if (!you.duration[DUR_TOXIC_RADIANCE])
        return false;

    if (crawl_state.disables[DIS_CONFIRMATIONS])
        return false;

    if (crawl_state.which_god_acting() == GOD_XOM)
        return false;

    string prompt = make_stringf("Really %s while emitting a toxic aura?",
                                 verb.c_str());

    if (yesno(prompt.c_str(), false, 'n'))
        return false;
    else
    {
        canned_msg(MSG_OK);
        return true;
    }
}
