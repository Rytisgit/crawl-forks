/**
 * @file
 * @brief Functions for making use of inventory items.
**/

#include "AppHdr.h"

#include "item-use.h"

#include "ability.h"
#include "acquire.h"
#include "act-iter.h"
#include "areas.h"
#include "artefact.h"
#include "art-enum.h"
#include "butcher.h"
#include "chardump.h"
#include "cloud.h"
#include "colour.h"
#include "coordit.h"
#include "database.h"
#include "decks.h"
#include "delay.h"
#include "describe.h"
#include "english.h"
#include "env.h"
#include "evoke.h"
#include "fight.h"
#include "food.h"
#include "god-conduct.h"
#include "god-item.h"
#include "god-passive.h"
#include "hints.h"
#include "invent.h"
#include "item-prop.h"
#include "item-status-flag-type.h"
#include "items.h"
#include "level-state-type.h"
#include "libutil.h"
#include "macro.h"
#include "makeitem.h"
#include "message.h"
#include "misc.h"
#include "mon-behv.h"
#include "mon-place.h"
#include "mutation.h"
#include "nearby-danger.h"
#include "orb.h"
#include "output.h"
#include "player.h"
#include "player-equip.h"
#include "player-stats.h"
#include "potion.h"
#include "prompt.h"
#include "religion.h"
#include "shout.h"
#include "skills.h"
#include "sound.h"
#include "spl-book.h"
#include "spl-clouds.h"
#include "spl-goditem.h"
#include "spl-selfench.h"
#include "spl-summoning.h"
#include "spl-transloc.h"
#include "spl-wpnench.h"
#include "state.h"
#include "stringutil.h"
#include "target.h"
#include "terrain.h"
#include "throw.h"
#include "tiles-build-specific.h"
#include "transform.h"
#include "uncancel.h"
#include "unwind.h"
#include "view.h"
#include "xom.h"

// The menu class for using items from either inv or floor.
// Derivative of InvMenu

class UseItemMenu : public InvMenu
{
    bool is_inventory = true;

    void populate_list(int item_type);
    void populate_menu();
    bool process_key(int key) override;

public:
    vector<const item_def*> item_inv;
    vector<const item_def*> item_floor;

    // Constructor
    // Requires int for item filter.
    // Accepts:
    //      OBJ_POTIONS
    //      OBJ_SCROLLS
    UseItemMenu(int selector, const char* prompt);

    void toggle();
};

UseItemMenu::UseItemMenu(int item_type, const char* prompt)
    : InvMenu(MF_SINGLESELECT)
{
    set_title(prompt);
    populate_list(item_type);
    populate_menu();
}

void UseItemMenu::populate_list(int item_type)
{
    // Load inv items first
    for (const auto &item : you.inv)
    {
        // Populate the vector with filter
        if (item.defined() && item_is_selected(item, item_type))
            item_inv.push_back(&item);
    }
    // Load floor items
    item_floor = item_list_on_square(you.visible_igrd(you.pos()));
    // Filter
    erase_if(item_floor, [=](const item_def* item)
    {
        return !item_is_selected(*item,item_type);
    });
}

void UseItemMenu::populate_menu()
{
    if (item_inv.empty())
        is_inventory = false;
    else if (item_floor.empty())
        is_inventory = true;

    if (!item_inv.empty())
    {
        // Only clarify that these are inventory items if there are also floor
        // items.
        if (!item_floor.empty())
        {
            string subtitle_text = "Inventory Items";
            if (!is_inventory)
                subtitle_text += " (',' to select)";
            auto subtitle = new MenuEntry(subtitle_text, MEL_TITLE);
            subtitle->colour = LIGHTGREY;
            add_entry(subtitle);
        }

        // nullptr means using the items' normal hotkeys
        if (is_inventory)
            load_items(item_inv);
        else
        {
            load_items(item_inv,
                        [&](MenuEntry* entry) -> MenuEntry*
                        {
                            entry->hotkeys.clear();
                            return entry;
                        });
        }
    }

    if (!item_floor.empty())
    {
#ifndef USE_TILE
        // vertical padding for console
        if (!item_inv.empty())
            add_entry(new MenuEntry("", MEL_TITLE));
#endif
        // Load floor items to menu
        string subtitle_text = "Floor Items";
        if (is_inventory)
            subtitle_text += " (',' to select)";
        auto subtitle = new MenuEntry(subtitle_text, MEL_TITLE);
        subtitle->colour = LIGHTGREY;
        add_entry(subtitle);

        // nullptr means using a-zA-Z
        if (is_inventory)
        {
            load_items(item_floor,
                        [&](MenuEntry* entry) -> MenuEntry*
                        {
                            entry->hotkeys.clear();
                            return entry;
                        });
        }
        else
            load_items(item_floor);
    }
}

void UseItemMenu::toggle()
{
    is_inventory = !is_inventory;
    deleteAll(items);
    populate_menu();
}

bool UseItemMenu::process_key(int key)
{
    if (isadigit(key) || key == '*' || key == '\\' || key == ',')
    {
        lastch = key;
        return false;
    }
    return Menu::process_key(key);
}

static string _weird_smell()
{
    return getMiscString("smell_name");
}

static string _weird_sound()
{
    return getMiscString("sound_name");
}

/**
 * Prompt use of a consumable from either player inventory or the floor.
 *
 * This function generates a menu containing type_expect items based on the
 * object_class_type to be acted on by another function. First it will list
 * items in inventory, then items on the floor. If player cancels out of menu,
 * nullptr is returned.
 *
 * @param item_type The object_class_type or OSEL_* of items to list.
 * @param oper The operation being done to the selected item.
 * @param prompt The prompt on the menu title
 * @param allowcancel If the user tries to cancel out of the prompt, run this
 *                    function. If it returns false, continue the prompt rather
 *                    than returning null.
 *
 * @return a pointer to the chosen item, or nullptr if none was chosen.
 */
item_def* use_an_item(int item_type, operation_types oper, const char* prompt,
                      function<bool ()> allowcancel)
{
    item_def* target = nullptr;

    // First handle things that will return nullptr

    // No selectable items in inv or floor
    if (!any_items_of_type(item_type, -1, true))
    {
        mprf(MSGCH_PROMPT, "%s",
             no_selectables_message(item_type).c_str());
        return nullptr;
    }

    // Init the menu
    UseItemMenu menu(item_type, prompt);

    while (true)
    {
        vector<MenuEntry*> sel = menu.show(true);
        int keyin = menu.getkey();

        // Handle inscribed item keys
        if (isadigit(keyin))
            target = digit_inscription_to_item(keyin, oper);
        // TODO: handle * key
        else if (keyin == ',')
        {
            if (Options.easy_floor_use && menu.item_floor.size() == 1)
                target = const_cast<item_def*>(menu.item_floor[0]);
            else
            {
                menu.toggle();
                continue;
            }
        }
        else if (keyin == '\\')
        {
            check_item_knowledge();
            continue;
        }
        else if (!sel.empty())
        {
            ASSERT(sel.size() == 1);

            auto ie = dynamic_cast<InvEntry *>(sel[0]);
            target = const_cast<item_def*>(ie->item);
        }

        redraw_screen();
        if (target && !check_warning_inscriptions(*target, oper))
            target = nullptr;
        if (target)
            return target;
        else if (allowcancel())
        {
            prompt_failed(PROMPT_ABORT);
            return nullptr;
        }
        else
            continue;
    }
}

static bool _handle_warning(const item_def &item)
{
    bool penance = false;
    if (needs_handle_warning(item, OPER_WIELD, penance))
    {
        string prompt =
            "Really unwield " + item.name(DESC_INVENTORY) + "?";
        if (penance)
            prompt += " This could place you under penance!";

        if (!yesno(prompt.c_str(), false, 'n'))
        {
            canned_msg(MSG_OK);
            return false;
        }
    }
    return true;
}

static bool _safe_to_remove_or_wear(const item_def &item, bool remove,
                                    bool quiet = false);

// Rather messy - we've gathered all the can't-wield logic from wield_weapon()
// here.
bool can_wield(const item_def *weapon, bool say_reason,
               bool ignore_temporary_disability, bool unwield, bool only_known)
{
#define SAY(x) {if (say_reason) { x; }}
    if (you.melded[EQ_WEAPON0] && unwield)
    {
        SAY(mpr("It's melded into your body!"));
        return false;
    } 

    if (!ignore_temporary_disability && !form_can_wield(you.form))
    {
        SAY(mpr("You can't wield anything in your present form."));
        return false;
    }

    if (!ignore_temporary_disability
        && you.weapon(0)
        && ((you.hands_reqd(*you.weapon(0)) == HANDS_TWO) || you.get_mutation_level(MUT_MISSING_HAND))
        && you.weapon(0)->soul_bound())
    {
        SAY(mprf("You can't unwield your %s%s!",
                you.weapon(0)->base_type == OBJ_SHIELDS ? "shield" : "weapon",
                 !unwield ? " to draw a new one" : ""));
        return false;
    }

    if (!ignore_temporary_disability
        && you.weapon(1)
        && (you.hands_reqd(*you.weapon(1)) == HANDS_TWO)
        && you.weapon(1)->soul_bound())
    {
        SAY(mprf("You can't unwield your %s%s!",
            you.weapon(1)->base_type == OBJ_SHIELDS ? "shield" : "weapon",
            !unwield ? " to draw a new one" : ""));
        return false;
    }

    if (!ignore_temporary_disability
        && you.weapon(0)
        && you.weapon(1)
        && you.weapon(0)->soul_bound()
        && you.weapon(1)->soul_bound())
    {
        SAY(mprf("You're soul bound to two items!"));
        return false;
    }

    // If we don't have an actual weapon to check, return now.
    if (!weapon)
        return true;

    if (you.get_mutation_level(MUT_MISSING_HAND)
            && you.hands_reqd(*weapon) == HANDS_TWO)
    {
        SAY(mpr("You can't wield that without your missing limb."));
        return false;
    }

    if (!you.could_wield(*weapon, false, false, !say_reason))
        return false;

    if (weapon->base_type != OBJ_WEAPONS && weapon->base_type != OBJ_STAVES
        && weapon->base_type != OBJ_SHIELDS && !weapon->is_type(OBJ_MISCELLANY, MISC_LANTERN_OF_SHADOWS))
    {
        SAY(mpr("You can't wield that."));
        return false;
    }

    if (!ignore_temporary_disability
             && you.hunger_state < HS_FULL
             && (get_weapon_brand(*weapon) == SPWPN_VAMPIRISM || is_unrandom_artefact(*weapon, UNRAND_MAJIN))
             && you.undead_state() == US_ALIVE
             && !you_foodless()
             && (item_type_known(*weapon) || !only_known)
             && !you.wearing_ego(EQ_GLOVES, SPARM_WIELDING))
    {
        if (say_reason)
            mpr("This weapon is vampiric, and you must be Full or above to equip it.");
        return false;
    }

    // We can wield this weapon. Phew!
    return true;

#undef SAY
}

// Returns a list of possible weapon slots.
static vector<equipment_type> _current_weapon_types()
{
    vector<equipment_type> ret;
    ret.push_back(EQ_WEAPON0);
    if (!you.get_mutation_level(MUT_MISSING_HAND))
        ret.push_back(EQ_WEAPON1);
    return ret;
}

static char _weapon_slot_key(equipment_type slot)
{
    switch (slot)
    {
    case EQ_WEAPON0:      return '<';
    case EQ_WEAPON1:      return '>';
    default:
        die("Invalid weapon slot");
    }
}

static int _prompt_weapon_to_unwield()
{
    const vector<equipment_type> weapon_types = _current_weapon_types();
    vector<char> slot_chars;
    vector<item_def*> weapons;
    for (auto eq : weapon_types)
    {
        weapons.push_back(you.slot_item(eq, true));
        ASSERT(weapons.back());
        slot_chars.push_back(index_to_letter(weapons.back()->link));
    }

    clear_messages();

    mprf(MSGCH_PROMPT,
        "You're wielding all the weapons you can. Unwield which one?");
    mprf(MSGCH_PROMPT, "(<w>?</w> for menu, <w>Esc</w> to cancel)");

    // FIXME: Needs TOUCH_UI version

    for (size_t i = 0; i < weapons.size(); i++)
    {
        string m = "<w>";
        const char key = _weapon_slot_key(weapon_types[i]);
        m += key;
        if (key == '<')
            m += '<';

        m += "</w> or " + weapons[i]->name(DESC_INVENTORY);
        mprf_nocap("%s", m.c_str());
    }
    flush_prev_message();

    // Deactivate choice from tile inventory.
    // FIXME: We need to be able to get the choice (item letter)n
    //        *without* the choice taking action by itself!
    int eqslot = EQ_NONE;

    mouse_control mc(MOUSE_MODE_PROMPT);
    int c;
    do
    {
        c = getchm();
        for (size_t i = 0; i < slot_chars.size(); i++)
        {
            if (c == slot_chars[i]
                || c == _weapon_slot_key(weapon_types[i]))
            {
                eqslot = weapon_types[i];
                c = ' ';
                break;
            }
        }
    } while (!key_is_escape(c) && c != ' ' && c != '?');

    clear_messages();

    if (c == '?')
        return EQ_NONE;
    else if (key_is_escape(c) || eqslot == EQ_NONE)
        return -2;

    return you.equip[eqslot];
}

/**
 * @param auto_wield false if this was initiated by the wield weapon command (w)
 *      true otherwise (e.g. switching between ranged and melee with the
 *      auto_switch option)
 * @param slot Index into inventory of item to equip. Or one of following
 *     special values:
 *      - -1 (default): meaning no particular weapon. We'll either prompt for a
 *        choice of weapon (if auto_wield is false) or choose one by default.
 *      - SLOT_BARE_HANDS: equip nothing (unwielding current weapon, if any)
 */
bool wield_weapon(bool auto_wield, int slot, bool show_weff_messages,
                  bool show_unwield_msg, bool show_wield_msg,
                  bool adjust_time_taken)
{
    if (inv_count() < 1)
    {
        canned_msg(MSG_NOTHING_CARRIED);
        return false;
    }

    // Look for conditions like berserking that could prevent wielding
    // weapons.
    if (!can_wield(nullptr, true, false, slot == SLOT_BARE_HANDS))
        return false;

    int item_slot = 0;          // default is 'a'

    // Now that quickswap is moved to its own function; auto-wield is
    // only used to equip items from their inventory description.
    if (auto_wield && slot != -1)
        item_slot = slot;

    // If the swap slot has a bad (but valid) item in it,
    // the swap will be to bare hands.
    const bool good_swap = (item_slot == SLOT_BARE_HANDS
                            || item_is_wieldable(you.inv[item_slot]));

    // Prompt if not using the auto swap command, or if the swap slot
    // is empty.
    if (item_slot != SLOT_BARE_HANDS
        && (!auto_wield || !you.inv[item_slot].defined() || !good_swap))
    {
        if (!auto_wield)
        {
            item_slot = prompt_invent_item(
                            "Wield which item (- for none, * to show all)?",
                            menu_type::invlist, OSEL_WIELD,
                            OPER_WIELD, invprompt_flag::no_warning, '-');
        }
        else
            item_slot = SLOT_BARE_HANDS;
    }

    if (prompt_failed(item_slot))
        return false;
    else if (item_slot == you.equip[EQ_CYTOPLASM])
    {
        if (!yesno("You need to eject it from your cytoplasm first. Continue?", true, 'n'))
            return false;
        if (!eject_item())
            return false;
    }
    else if (item_slot == you.equip[EQ_WEAPON0]
        || item_slot == you.equip[EQ_WEAPON1])
    {
        if (Options.equip_unequip)
        {
            item_slot = SLOT_BARE_HANDS;
        }
        else
        {
            if (you.get_mutation_level(MUT_MISSING_HAND))
            {
                mpr("You have no other limb to swap to...");
                return false;
            }
            if ((you.weapon(0) && you.inv[you.equip[EQ_WEAPON0]].soul_bound()) 
                || (you.weapon(1) && you.inv[you.equip[EQ_WEAPON1]].soul_bound()))
            {
                mpr("You can't swap weapons while either is bound to your soul.");
                return false;
            }
            if ((you.weapon(0) && you.inv[you.equip[EQ_WEAPON0]].fragile())
                || (you.weapon(1) && you.inv[you.equip[EQ_WEAPON1]].fragile()))
            {
                mpr("You can't swap weapons while either is fragile. You'd simply destroy the fragile one.");
                return false;
            }
            if (you.weapon(0) && is_range_weapon(*you.weapon(0)))
            {
                mpr("Ranged weapons can only be wielded in your right hand.");
                return false;
            }
            if (you.weapon(0) && you.hands_reqd(*you.weapon(0)) == HANDS_TWO)
            {
                mpr("You're already wielding that.");
                return false;
            }
            if (yesno("Switch to other hand?", true, false)) 
            {
                int temp0 = you.equip[EQ_WEAPON0];
                int temp1 = you.equip[EQ_WEAPON1];
                if (you.weapon(0))
                {
                    if (!can_wield(you.weapon(0),true,false,false,false))
                        return false;
                    if (!_handle_warning(*you.weapon(0)))
                        return false;
                }
                if (you.weapon(1))
                {
                    if (!can_wield(you.weapon(1), true, false, false, false))
                        return false;
                    if (!_handle_warning(*you.weapon(1)))
                        return false;
                }
                
                if (you.weapon(0))
                    unequip_item(EQ_WEAPON0, show_weff_messages);
                if (you.weapon(1))
                    unequip_item(EQ_WEAPON1, show_weff_messages);

                if (temp1 > -1)
                    equip_item(EQ_WEAPON0, temp1, show_weff_messages);
                if (temp0 > -1)
                    equip_item(EQ_WEAPON1, temp0, show_weff_messages);

                if (adjust_time_taken)
                    you.time_taken /= 2;

                you.wield_change = true;
                you.m_quiver.on_weapon_changed();
                you.turn_is_over = true;
            }

            else
                canned_msg(MSG_OK);
            return true;
        }
    }

    // Reset the warning counter.
    you.received_weapon_warning = false;

    int unwanted = 0;
    item_def* wpn;

    if (item_slot == SLOT_BARE_HANDS)
    {
        if (you.weapon(0))
        {
            if (you.weapon(1))
            {
                if (you.weapon(1)->soul_bound())
                    wpn = you.weapon(0);
                else if (you.weapon(0)->soul_bound())
                    wpn = you.weapon(1);
                else if (you.get_mutation_level(MUT_MISSING_HAND))
                    wpn = you.weapon(0);
                else if (auto_wield)
                {
                    if (is_range_weapon(*you.weapon(0)) || (you.weapon(0)->base_type == OBJ_SHIELDS && !is_hybrid(you.weapon(0)->sub_type)))
                    {
                        if (you.weapon(1)->base_type == OBJ_SHIELDS && !is_hybrid(you.weapon(1)->sub_type))
                            wpn = you.weapon(0);
                        else 
                            wpn = you.weapon(1);
                    }
                    else
                        wpn = you.weapon(0);
                }
                else
                {
                    unwanted = _prompt_weapon_to_unwield();

                    if (unwanted == EQ_NONE)
                    {
                        unwanted = prompt_invent_item(
                            "You're wielding something in both hands. Unwield which one?",
                            menu_type::invlist, OSEL_UNCURSED_WIELDED_WEAPONS, OPER_REMOVE,
                            invprompt_flag::no_warning | invprompt_flag::hide_known);;
                    }

                    if (unwanted < 0)
                    {
                        canned_msg(MSG_OK);
                        return false;
                    }

                    wpn = &you.inv[unwanted];
                }
            }

            else
            {
                wpn = you.weapon(0);
            }
        }

        else if (you.weapon(1) && !you.weapon(0))
        {
            wpn = you.weapon(1);
        }
        
        else
        {
            canned_msg(MSG_EMPTY_HANDED_ALREADY);
            return false;
        }

        if (!_handle_warning(*wpn))
            return false;

        // check if you'd get stat-zeroed
        if (!_safe_to_remove_or_wear(*wpn, true))
            return false;

        if (wpn->soul_bound())
        {
            mprf("You can't unwield your %s.", wpn->base_type == OBJ_SHIELDS ? "shield" : "weapon");
            return false;
        }

        if (wpn == you.weapon(0))
        {
            if (!unwield_item(true, show_weff_messages))
                return false;
        }

        else
        {
            if (!unwield_item(false, show_weff_messages))
                return false;
        }

        if (show_unwield_msg)
        {
#ifdef USE_SOUND
            parse_sound(WIELD_NOTHING_SOUND);
#endif
            canned_msg(MSG_EMPTY_HANDED_NOW);
        }

        // Switching to bare hands is extra fast.
        you.turn_is_over = true;
        if (adjust_time_taken)
        {
            you.time_taken *= 3;
            you.time_taken /= 10;
        }
        return true;
    }

    item_def& new_wpn(you.inv[item_slot]);

    // Switching to a launcher while berserk is likely a mistake.
    if (you.berserk() && is_range_weapon(new_wpn))
    {
        string prompt = "You can't shoot while berserk! Really wield " +
                        new_wpn.name(DESC_INVENTORY) + "?";
        if (!yesno(prompt.c_str(), false, 'n'))
        {
            canned_msg(MSG_OK);
            return false;
        }
    }

    // Ensure wieldable
    if (!can_wield(&new_wpn, true))
        return false;

    // Really ensure wieldable, even unknown brand
    if (!can_wield(&new_wpn, true, false, false, false))
        return false;

    // At this point, we know it's possible to equip this item. However, there
    // might be reasons it's not advisable.
    if (!_safe_to_remove_or_wear(new_wpn, false))
    {
        canned_msg(MSG_OK);
        return false;
    }

    bool lopen = false;
    if (needs_handle_warning(new_wpn, OPER_WIELD, lopen))
    {
        string prompt =
            "Really wield " + new_wpn.name(DESC_INVENTORY) + "?";
        if (lopen)
            prompt += " This could place you under penance!";

        if (!yesno(prompt.c_str(), false, 'n'))
        {
            canned_msg(MSG_OK);
            return false;
        }
    }

    if (new_wpn.base_type == OBJ_STAVES && you.slot_item(EQ_CYTOPLASM) && you.slot_item(EQ_CYTOPLASM)->base_type == OBJ_STAVES)
    {
        if (!yesno("You can only attune to one staff at a time. Eject your current staff?", true, 'n'))
        {
            canned_msg(MSG_OK);
            return false;
        }
        eject_item();
    }

    if (you.hands_reqd(new_wpn) == HANDS_TWO)
    {
        if (you.weapon(0))
        {
            if (you.weapon(0)->soul_bound())
            {
                mpr("You can't unwield what's in your right hand.");
                return false;
            }
            if (you.weapon(1) && you.weapon(1)->soul_bound())
            {
                mpr("You can't unwield what's in your left hand.");
                return false;
            }
            
            if (!_handle_warning(*you.weapon(0)))
                return false;
            if (you.weapon(1) && !_handle_warning(*you.weapon(1)))
                return false;
            if (!unwield_item(true, show_weff_messages))
                return false;
        }
        if (you.weapon(1))
        {
            if (you.weapon(1)->soul_bound())
            {
                mpr("You can't unwield what's in your left hand.");
                return false;
            }
            if (!_handle_warning(*you.weapon(1)))
                return false;

            if (!unwield_item(false, show_weff_messages))
                return false;
        }
        update_can_currently_train();
        // Go ahead and wield the weapon.
        equip_item(EQ_WEAPON0, item_slot, show_weff_messages);
    }

    else if (is_range_weapon(new_wpn))
    {
        if (you.weapon(0) && you.weapon(0)->soul_bound())
        {
            mpr("You can't unwield what's in your right hand.");
            return false;
        }

        if (!you.weapon(0))
            equip_item(EQ_WEAPON0, item_slot, show_weff_messages);

        if (!_handle_warning(*you.weapon(0)))
            return false;

        if (!unwield_item(true, show_weff_messages))
            return false;

        equip_item(EQ_WEAPON0, item_slot, show_weff_messages);
    }

    else if (new_wpn.base_type == OBJ_STAVES && you.weapon(0) && you.weapon(0)->base_type == OBJ_STAVES)
    {
        if (you.weapon(0)->soul_bound())
        {
            mpr("You can't unwield your magical staff to draw a new one.");
            return false;
        }

        if (!_handle_warning(*you.weapon(0)))
            return false;

        if (!unwield_item(true, show_weff_messages))
            return false;

        equip_item(EQ_WEAPON0, item_slot, show_weff_messages);
    }

    else if (new_wpn.base_type == OBJ_STAVES && you.weapon(1) && you.weapon(1)->base_type == OBJ_STAVES)
    {
        if (you.weapon(1)->soul_bound())
        {
            mpr("You can't unwield your magical staff to draw a new one.");
            return false;
        }

        if (!_handle_warning(*you.weapon(1)))
            return false;

        if (!unwield_item(false, show_weff_messages))
            return false;

        equip_item(EQ_WEAPON1, item_slot, show_weff_messages);
    }

    else if (!you.weapon(0))
    {
        equip_item(EQ_WEAPON0, item_slot, show_weff_messages);
    }

    else if (you.get_mutation_level(MUT_MISSING_HAND) && you.weapon(0))
    {
        if (!_handle_warning(*you.weapon(0)))
            return false;
        if (unwield_item(true, show_weff_messages))
            equip_item(EQ_WEAPON0, item_slot, show_weff_messages);
        else
            return false;
    }

    else if (!you.weapon(1))
    {
        if (you.hands_reqd(*(you.weapon(0))) == HANDS_TWO)
        {
            if (!_handle_warning(*you.weapon(0)))
                return false;
            if (unwield_item(true, show_weff_messages))
                equip_item(EQ_WEAPON0, item_slot, show_weff_messages);
            else
                return false;
        }
        else
            equip_item(EQ_WEAPON1, item_slot, show_weff_messages);
    }

    else if (you.weapon(0)->soul_bound())
    {
        if (!_handle_warning(*you.weapon(1)))
            return false;
        if (unwield_item(false, show_weff_messages))
            equip_item(EQ_WEAPON1, item_slot, show_weff_messages);
    }

    else if (you.weapon(1)->soul_bound())
    {
        if (!_handle_warning(*you.weapon(0)))
            return false;
        if (unwield_item(true, show_weff_messages))
            equip_item(EQ_WEAPON0, item_slot, show_weff_messages);
    }

    else if (auto_wield)
    {
        if (is_range_weapon(*you.weapon(0)) || (you.weapon(0)->base_type == OBJ_SHIELDS && !is_hybrid(you.weapon(0)->sub_type)))
        {
            if (you.weapon(1)->base_type == OBJ_SHIELDS && !is_hybrid(you.weapon(1)->sub_type))
            {
                if (!_handle_warning(*you.weapon(0)))
                    return false;
                if (unwield_item(true, show_weff_messages))
                    equip_item(EQ_WEAPON0, item_slot, show_weff_messages);
            }
            else
            {
                if (!_handle_warning(*you.weapon(1)))
                    return false;
                else if (unwield_item(false, show_weff_messages))
                    equip_item(EQ_WEAPON1, item_slot, show_weff_messages);
            }
        }
        else
        {
            if (!_handle_warning(*you.weapon(0)))
                return false;
            else if (unwield_item(true, show_weff_messages))
                equip_item(EQ_WEAPON0, item_slot, show_weff_messages);
        }
    }

    else
    {
        unwanted = _prompt_weapon_to_unwield();

        if (unwanted == EQ_NONE)
        {
            unwanted = prompt_invent_item(
                "You're wielding something in both hands. Unwield which one?",
                menu_type::invlist, OSEL_UNCURSED_WIELDED_WEAPONS, OPER_REMOVE,
                invprompt_flag::no_warning | invprompt_flag::hide_known);;
        }

        if (unwanted < 0)
        {
            canned_msg(MSG_OK);
            return false;
        }

        wpn = &you.inv[unwanted];
        // check if you'd get stat-zeroed

        if (!_safe_to_remove_or_wear(*wpn, true))
            return false;

        if (!_handle_warning(*wpn))
            return false;

        if (wpn == you.weapon(0))
        {
            if (unwield_item(true, show_weff_messages))
                equip_item(EQ_WEAPON0, item_slot, show_weff_messages);
            else
                return false;
        }
        
        else
        {
            if (unwield_item(false, show_weff_messages))
                equip_item(EQ_WEAPON1, item_slot, show_weff_messages);
            else
                return false;
        }
    }

    const unsigned int old_talents = your_talents(false).size();

    if (show_wield_msg)
    {
#ifdef USE_SOUND
        parse_sound(WIELD_WEAPON_SOUND);
#endif
        mprf_nocap("%s", new_wpn.name(DESC_INVENTORY_EQUIP).c_str());
    }

    check_item_hint(new_wpn, old_talents);

    // Time calculations.
    if (adjust_time_taken)
        you.time_taken /= 2;

    you.wield_change  = true;
    you.m_quiver.on_weapon_changed();
    you.turn_is_over  = true;

    return true;
}

bool item_is_worn(int inv_slot)
{
    if (you.get_mutation_level(MUT_AMORPHOUS_BODY))
    {
        for (int i = EQ_FIRST_MORPH; i <= EQ_LAST_MORPH; ++i)
            if (inv_slot == you.equip[i])
                return true;
    }
    else
    {
        for (int i = EQ_MIN_ARMOUR; i <= EQ_MAX_WORN; ++i)
            if (inv_slot == you.equip[i])
                return true;
    }

    return false;
}

/**
 * Prompt user for carried armour.
 *
 * @param mesg Title for the prompt
 * @param index[out] the inventory slot of the item chosen; not initialised
 *                   if a valid item was not chosen.
 * @param oper if equal to OPER_TAKEOFF, only show items relevant to the 'T'
 *             command.
 * @return whether a valid armour item was chosen.
 */
bool armour_prompt(const string & mesg, int *index, operation_types oper)
{
    ASSERT(index != nullptr);

    if (you.berserk())
        canned_msg(MSG_TOO_BERSERK);
    else
    {
        int selector = OBJ_ARMOURS;
        if (oper == OPER_TAKEOFF && !Options.equip_unequip)
            selector = OSEL_WORN_ARMOUR;
        int slot = prompt_invent_item(mesg.c_str(), menu_type::invlist,
                                      selector, oper);

        if (!prompt_failed(slot))
        {
            *index = slot;
            return true;
        }
    }

    return false;
}

// Rework of the quick_swap command to be more dual-wield friendly. Newly finicky 
// to prevent unexpected behaviors.
bool quick_swap()
{
    const item_def wpn0 = you.inv[0]; // a
    const item_def wpn1 = you.inv[1]; // b

    if (!wpn0.defined() || !wpn1.defined())
    {
        mprf(MSGCH_ERROR, "One of your quick swap slots is empty. Adjust and try again.");
        return false;
    }

    if (!you.could_wield(wpn0))
    {
        mprf(MSGCH_ERROR, "You cannot wield what you have in slot 'a'. Adjust and try again.");
        return false;
    }

    if (!you.could_wield(wpn1))
    {
        mprf(MSGCH_ERROR, "You cannot wield what you have in slot 'b'. Adjust and try again.");
        return false;
    }

    if (wpn0.soul_bound() || wpn1.soul_bound())
    {
        mprf(MSGCH_ERROR, "You cannot swap items while one is bound to your soul.");
        return false;
    }

    if (!can_wield(&wpn0, true, false, false, false))
        return false;

    if (!can_wield(&wpn1, true, false, false, false))
        return false;

    int swapto = -1;
    int swapfrom = -1;

    if (you.equip[EQ_WEAPON0] == 0)
    {
        swapto = EQ_WEAPON0;
        swapfrom = 1;
    }
    else if (you.equip[EQ_WEAPON0] == 1)
    {
        swapto = EQ_WEAPON0;
        swapfrom = 0;
    }
    else if (you.equip[EQ_WEAPON1] == 0)
    {
        swapto = EQ_WEAPON1;
        swapfrom = 1;
    }
    else if (you.equip[EQ_WEAPON1] == 1)
    {
        swapto = EQ_WEAPON1;
        swapfrom = 0;
    }

    if (swapto < 0)
    {
        mprf("Can only swap items when wielding one of the items you intend to swap.");
        return false;
    }

    if (swapto == EQ_WEAPON1 && is_range_weapon(you.inv[swapfrom]))
    {
        mprf(MSGCH_ERROR, "Can't swap a ranged weapon into the left hand slot.");
        return false;
    }

    if (you.staff() && you.inv[swapto].base_type == OBJ_STAVES
        && !(wpn0.base_type == OBJ_STAVES && wpn1.base_type == OBJ_STAVES))
    {
        mprf(MSGCH_ERROR, "This swap would lead to wielding two staves at once. You can't do this.");
        return false;
    }

    if (you.hands_reqd(you.inv[swapto]) > HANDS_ONE && you.weapon(0) && you.weapon(1))
    {
        mprf(MSGCH_ERROR, "Can't quickswap to a two-handed item while wielding two items.");
        return false;
    }

    if (!unwield_item(!swapto))
        return false;

    equip_item(static_cast<equipment_type>(swapto), swapfrom, true);

    you.wield_change = true;
    you.m_quiver.on_weapon_changed();
    you.turn_is_over = true;

    return true;
}

// Double Swap. Intentionally a bit finicky because is it'd be hard to logic out
// both "what the player wants this to do" and "should this be able to work"
// if it wasn't finicky. It 'errors' to tell the player why they can't double
// swap if they don't fit the function's requirements.
bool double_swap()
{
    if ((you.weapon(0) && you.weapon(0)->soul_bound()) ||
        (you.weapon(1) && you.weapon(1)->soul_bound()))
    {
        mpr("You can't dualswap while soul bound to a weapon!");
        return false;
    }

    int item_slot0 = 0; // a
    int item_slot1 = 1; // b

    if (item_slot0 == you.equip[EQ_WEAPON0] || item_slot0 == you.equip[EQ_WEAPON1]
        || item_slot1 == you.equip[EQ_WEAPON0] || item_slot1 == you.equip[EQ_WEAPON1])
    {
        item_slot0 = 2; // c
        item_slot1 = 3; // d
    }

    if (item_slot0 == you.equip[EQ_WEAPON0] || item_slot0 == you.equip[EQ_WEAPON1]
        || item_slot1 == you.equip[EQ_WEAPON0] || item_slot1 == you.equip[EQ_WEAPON1])
    {
        mpr("You're already wielding some of both your sets of dual swap items.");
        return false;
    }

    item_def wpn0 = you.inv[item_slot0];

    if (!wpn0.defined())
    {
        mpr("One of your dualswap slots is empty.");
        return false;
    }

    if (!can_wield(&wpn0, true, false, false, false))
        return false;

    if (you.hands_reqd(wpn0) == HANDS_TWO)
    {
        if (you.weapon(0))
        {
            if (!unwield_item(true, true))
                return false;
        }
        if (you.weapon(1))
        {
            if (!unwield_item(false, true))
                return false;
        }
        equip_item(EQ_WEAPON0, item_slot0, true);
        return true;
    }

    wpn0 = you.inv[item_slot1];

    if (!wpn0.defined())
    {
        mpr("One of your dualswap slots is empty.");
        return false;
    }

    if (!can_wield(&wpn0, true, false, false, false))
        return false;

    if (you.hands_reqd(wpn0) == HANDS_TWO)
    {
        mpr ("You have a two-handed weapon in the wrong dualswap slot.");
        return false;
    }

    if (is_range_weapon(wpn0))
    {
        mpr("You have a ranged weapon in the wrong dualswap slot.");
        return false;
    }

    if (you.weapon(0))
    {
        if (!unwield_item(true, true))
            return false;
    }
    if (you.weapon(1))
    {
        if (!unwield_item(false, true))
            return false;
    }

    equip_item(EQ_WEAPON0, item_slot0, true);
    equip_item(EQ_WEAPON1, item_slot1, true);

    you.wield_change = true;
    you.m_quiver.on_weapon_changed();
    you.turn_is_over = true;

    return true;
}

/**
 * The number of turns it takes to put on or take off a given piece of armour.
 *
 * @param item      The armour in question.
 * @return          The number of turns it takes to don or doff the item.
 */
static int armour_equip_delay(const item_def &/*item*/)
{
    return 5;
}

/**
 * Can you wear this item of armour currently?
 *
 * Ignores whether or not an item is equipped in its slot already.
 * If the item is Lear's hauberk, some of this comment may be incorrect.
 *
 * @param item The item. Only the base_type and sub_type really should get
 *             checked, since you_can_wear passes in a dummy item.
 * @param verbose Whether to print a message about your inability to wear item.
 * @param ignore_temporary Whether to take into account forms/fishtail/2handers.
 *                         Note that no matter what this is set to, all
 *                         mutations will be taken into account, except for
 *                         ones from Beastly Appendage, which are only checked
 *                         if this is false.
 */
bool can_wear_armour(const item_def &item, bool verbose, bool ignore_temporary)
{
    const object_class_type base_type = item.base_type;

    if (base_type != OBJ_ARMOURS)
    {
        if (verbose)
            mpr("You can't wear that.");

        return false;
    }

    if (you.get_mutation_level(MUT_AMORPHOUS_BODY))
        return true;

    const int sub_type = item.sub_type;
    const equipment_type slot = get_armour_slot(item);

    // Jiyva override!
    if (you.get_mutation_level(MUT_CORE_MELDING) && slot == EQ_BODY_ARMOUR)
        return true;

    if (you.species == SP_FELID || you.species == SP_FAIRY)
    {
        if (verbose)
            mpr("You can't wear that.");

        return false;
    }

    if (you.get_mutation_level(MUT_DEFORMED) > 1 && slot == EQ_BODY_ARMOUR)
    {
        if (verbose)
        {
            if (you.species == SP_DRACONIAN)
                mpr("Your wings won't fit in that.");
            else
                mpr("You can't fit into that, misshapen as you are.");
        }
        return false;
    }

    if (you.species == SP_LIGNIFITE && slot == EQ_BODY_ARMOUR && fit_armour_size(item, SIZE_LARGE) != 0)
    {
        if (verbose)
            mprf("Your branches won't fit in that!");
        return false;
    }

    if (sub_type == ARM_NAGA_BARDING || sub_type == ARM_CENTAUR_BARDING)
    {
        if ((you.species == SP_NAGA || you.char_class == JOB_NAGA) && sub_type == ARM_NAGA_BARDING
            || (you.species == SP_CENTAUR || you.char_class == JOB_CENTAUR) && sub_type == ARM_CENTAUR_BARDING
            || you.get_mutation_level(MUT_GELATINOUS_TAIL))
        {
            if (ignore_temporary || !player_is_shapechanged())
                return true;
            else if (verbose)
                mpr("You can wear that only in your normal form.");
        }
        else if (verbose)
            mpr("You can't wear that!");
        return false;
    }

    if (you.species == SP_OCTOPODE && slot != EQ_HELMET)
    {
        if (verbose)
            mpr("You can't wear that!");
        return false;
    }

    // Lear's hauberk covers also head, hands and legs.
    if (is_unrandom_artefact(item, UNRAND_LEAR))
    {
        if (!player_has_feet(!ignore_temporary))
        {
            if (verbose)
                mpr("You have no feet.");
            return false;
        }

        if (you.get_mutation_level(MUT_CLAWS, !ignore_temporary) >= 2)
        {
            if (verbose)
            {
                mprf("The hauberk won't fit your %s.",
                     you.hand_name(true).c_str());
            }
            return false;
        }

        if (you.get_mutation_level(MUT_ANTENNAE, !ignore_temporary))
        {
            if (verbose)
                mpr("The hauberk won't fit your head.");
            return false;
        }

        if (!ignore_temporary)
        {
            for (int s = EQ_HELMET; s <= EQ_BOOTS; s++)
            {
                // No strange race can wear this.
                const string parts[] = { "head", you.hand_name(true),
                                         you.foot_name(true) };
                COMPILE_CHECK(ARRAYSZ(parts) == EQ_BOOTS - EQ_HELMET + 1);

                // Auto-disrobing would be nice.
                if (you.equip[s] != -1)
                {
                    if (verbose)
                    {
                        mprf("You'd need your %s free.",
                             parts[s - EQ_HELMET].c_str());
                    }
                    return false;
                }

                if (!get_form()->slot_available(s))
                {
                    if (verbose)
                    {
                        mprf("The hauberk won't fit your %s.",
                             parts[s - EQ_HELMET].c_str());
                    }
                    return false;
                }
            }
        }
    }

    else if (slot >= EQ_HELMET && slot <= EQ_BOOTS
             && !ignore_temporary
             && player_equip_unrand(UNRAND_LEAR))
    {
        // The explanation is iffy for loose headgear, especially crowns:
        // kings loved hooded hauberks, according to portraits.
        if (verbose)
            mpr("You can't wear this over your hauberk.");
        return false;
    }

    size_type player_size = you.body_size(PSIZE_TORSO, ignore_temporary);
    int bad_size = fit_armour_size(item, player_size);
#if TAG_MAJOR_VERSION == 34
    if (is_unrandom_artefact(item, UNRAND_TALOS))
    {
        // adjust bad_size for the oversized plate armour
        // negative means levels too small, positive means levels too large
        bad_size = SIZE_LARGE - player_size;
    }
#endif

    if (you.form == transformation::appendage
        && ignore_temporary
        && slot == beastly_slot(you.attribute[ATTR_APPENDAGE])
        && you.has_mutation(static_cast<mutation_type>(you.attribute[ATTR_APPENDAGE])))
    {
        unwind_var<uint8_t> mutv(you.mutation[you.attribute[ATTR_APPENDAGE]], 0);
        // disable the mutation then check again
        return can_wear_armour(item, verbose, ignore_temporary);
    }

    if (sub_type == ARM_GLOVES || sub_type == ARM_CLAW)
    {
        if (you.has_claws(false) >= 2)
        {
            if (verbose)
            {
                mprf("You can't wear a glove with your huge claw%s!",
                     you.get_mutation_level(MUT_MISSING_HAND) ? "" : "s");
            }
            return false;
        }
        if (you.species == SP_LIGNIFITE)
        {
            if (verbose)
                mpr("Gloves don't fit on your branches!");
            return false;
        }
    }

    if (sub_type == ARM_BOOTS)
    {
        if (you.get_mutation_level(MUT_HOOVES, false))
        {
            if (verbose)
                mpr("You can't wear boots with hooves!");
            return false;
        }

        if (you.get_mutation_level(MUT_FROG_LEGS, false))
        {
            if (verbose)
                mpr("You can't wear boots with large webbed feet!");
            return false;
        }

        if (you.has_talons(false))
        {
            if (verbose)
                mpr("Boots don't fit your talons!");
            return false;
        }

        if (you.species == SP_NAGA || you.char_class == JOB_NAGA)
        {
            if (verbose)
                mpr("You have no legs!");
            return false;
        }

        if (you.species == SP_LIGNIFITE || you.get_mutation_level(MUT_ROOTS))
        {
            if (verbose)
                mpr("Boots don't fit on your roots!");
            return false;
        }

        if (!ignore_temporary && you.fishtail)
        {
            if (verbose)
                mpr("You don't currently have feet!");
            return false;
        }
    }

    if (slot == EQ_HELMET)
    {
        // Antennae disallow all headgear
        if (you.get_mutation_level(MUT_ANTENNAE, false) == 3)
        {
            if (verbose)
                mpr("You can't wear any headgear with your antennae!");
            return false;
        }

        // Soft helmets (caps and wizard hats) always fit, otherwise.
        if (is_hard_helmet(item))
        {
            if (you.get_mutation_level(MUT_HORNS, false))
            {
                if (verbose)
                    mpr("You can't wear that with your horns!");
                return false;
            }

            if (you.get_mutation_level(MUT_BEAK, false))
            {
                if (verbose)
                    mpr("You can't wear that with your beak!");
                return false;
            }

            if (species_is_draconian(you.species))
            {
                if (verbose)
                    mpr("You can't wear that with your reptilian head.");
                return false;
            }

            if (you.species == SP_LIGNIFITE)
            {
                if (verbose)
                    mpr("You can't wear that on your branches.");
                return false;
            }

            if (you.species == SP_OCTOPODE)
            {
                if (verbose)
                    mpr("You can't wear that!");
                return false;
            }
        }
    }

    if (bad_size)
    {
        if (verbose)
        {
            mprf("This armour is too %s for you!",
                (bad_size > 0) ? "big" : "small");
        }

        return false;
    }

    // Can't just use Form::slot_available because of shroom caps.
    if (!ignore_temporary && !get_form()->can_wear_item(item))
    {
        if (verbose)
            mpr("You can't wear that in your present form.");
        return false;
    }

    return true;
}

static bool _jester_check(const item_def &item, const bool remove)
{
    if (is_unrandom_artefact(item, UNRAND_JESTER_CAP))
    {
        string prompt = "";

        if (remove)
            prompt = "Removing the Jester's Cap will anger Xom! Continue?";

        else
        {
            prompt = make_stringf("Wearing the Jester's Cap will subject you "
                "to the capriciousness of Xom. Continue?");
        }

        if (!yesno(prompt.c_str(), true, 'n'))
        {
            canned_msg(MSG_OK);
            return false;
        }
    }
    return true;
}

static bool _can_takeoff_armour(int item, bool do_jester = true);

// Like can_wear_armour, but also takes into account currently worn equipment.
// e.g. you may be able to *wear* that robe, but you can't equip it if your
// currently worn armour is cursed, or melded.
// precondition: item is not already worn
static bool _can_equip_armour(const item_def &item)
{
    const object_class_type base_type = item.base_type;
    if (base_type != OBJ_ARMOURS)
    {
        mpr("You can't wear that.");
        return false;
    }

    if (!_jester_check(item, false))
        return false;

    if (you.get_mutation_level(MUT_AMORPHOUS_BODY))
    {
        int count = 0;
        int unavailable = 0;
        for (int i = EQ_FIRST_MORPH; i <= EQ_LAST_MORPH; i++)
        {
            count += 1;

            item_def * worn = you.slot_item(static_cast<equipment_type>(i));

            if (worn && !_can_takeoff_armour(worn->link, false))
            {
                equipment_type type = get_armour_slot(static_cast<armour_type>(worn->sub_type));
                unavailable += (type == EQ_BODY_ARMOUR || type == EQ_BARDING) ? 2 : 1;
            }
        }
        if (unavailable >= count)
            return false;
    }
    else
    {
        const equipment_type slot = get_armour_slot(item);
        const int equipped = you.equip[slot];
        if (equipped != -1 && !_can_takeoff_armour(equipped))
            return false;
    }
    return can_wear_armour(item, true, false);
}

// Try to equip the armour in the given inventory slot (or, if slot is -1,
// prompt for a choice of item, then try to wear it).
bool wear_armour(int item)
{
    // Before (possibly) prompting for which item to wear, check for some
    // conditions that would make it impossible to wear any type of armour.
    // TODO: perhaps also worth checking here whether all available armour slots
    // are cursed. Same with jewellery.
    if ((you.species == SP_FELID || you.species == SP_FAIRY) && !you.get_mutation_level(MUT_CORE_MELDING))
    {
        mpr("You can't wear anything.");
        return false;
    }

    if (!form_can_wear())
    {
        mpr("You can't wear anything in your present form.");
        return false;
    }

    if (you.berserk())
    {
        canned_msg(MSG_TOO_BERSERK);
        return false;
    }

    if (item == -1)
    {
        item = prompt_invent_item("Wear which item?", menu_type::invlist,
                                  OBJ_ARMOURS, OPER_WEAR,
                                  invprompt_flag::no_warning);
        if (prompt_failed(item))
            return false;
    }

    item_def &invitem = you.inv[item];
    // First, let's check for any conditions that would make it impossible to
    // equip the given item
    if (!invitem.defined())
    {
        mpr("You don't have any such object.");
        return false;
    }

    if (item_is_worn(item))
    {
        if (Options.equip_unequip)
            // TODO: huh? Why are we inverting the return value?
            return !takeoff_armour(item);
        else
        {
            mpr("You're already wearing that object!");
            return false;
        }
    }

    if (!_can_equip_armour(invitem))
        return false;

    // At this point, we know it's possible to equip this item. However, there
    // might be reasons it's not advisable. Warn about any dangerous
    // inscriptions, giving the player an opportunity to bail out.
    if (!check_warning_inscriptions(invitem, OPER_WEAR))
    {
        canned_msg(MSG_OK);
        return false;
    }

    int swapping = 0;
    const equipment_type slot = get_armour_slot(invitem);

    if (item == you.equip[EQ_CYTOPLASM])
    {
        if (!yesno("You need to eject it from your cytoplasm first. Continue?", true, 'n'))
            return false;
        if (!eject_item())
            return false;
    }

    if (you.get_mutation_level(MUT_AMORPHOUS_BODY))
    {
        int free_slots = 0;
        for (int i = EQ_FIRST_MORPH; i <= EQ_LAST_MORPH; i++)
        {
            item_def * worn = you.slot_item(static_cast<equipment_type>(i));

            if (!worn)
            {
                free_slots++;
                continue;
            }

            const equipment_type wslot = get_armour_slot(static_cast<armour_type>(worn->sub_type));
            if (wslot == EQ_BODY_ARMOUR || wslot == EQ_BARDING)
                free_slots--;
        }

        while (free_slots < ((slot == EQ_BODY_ARMOUR || slot == EQ_BARDING) ? 2 : 1))
        {
            if (crawl_state.seen_hups)
                return false;

            mprf("You can't put this on without taking something off first.");

            int remov = prompt_invent_item("Take off which item?", menu_type::invlist,
                            OBJ_ARMOURS, OPER_TAKEOFF, invprompt_flag::no_warning);

            if (prompt_failed(remov))
                return false;

            if (!takeoff_armour(remov))
                return false;

            equipment_type rslot = get_armour_slot(you.inv[remov]);

            if (rslot == EQ_BODY_ARMOUR || rslot == EQ_BARDING)
                free_slots++;

            free_slots++;
            swapping++;
        }
    }
    else
    {
        if ((slot == EQ_CLOAK
            || slot == EQ_HELMET
            || slot == EQ_GLOVES
            || slot == EQ_BOOTS
            || slot == EQ_BODY_ARMOUR
            || slot == EQ_BARDING)
            && you.equip[slot] != -1)
        {
            if (!takeoff_armour(you.equip[slot]))
                return false;
            swapping++;
        }
    }

    you.turn_is_over = true;

    // TODO: It would be nice if we checked this before taking off the item
    // currently in the slot. But doing so is not quite trivial. Also applies
    // to jewellery.
    if (!_safe_to_remove_or_wear(invitem, false))
        return false;

    const int delay = armour_equip_delay(invitem);
    if (delay)
        start_delay<ArmourOnDelay>(delay + swapping - 1, invitem);

    return true;
}

static bool _can_takeoff_armour(int item, bool do_jester)
{
    const item_def& invitem = you.inv[item];
    if (invitem.base_type != OBJ_ARMOURS)
    {
        mpr("You aren't wearing that!");
        return false;
    }

    if (you.berserk())
    {
        canned_msg(MSG_TOO_BERSERK);
        return false;
    }

    if (do_jester && !_jester_check(invitem, true))
        return false;

    const equipment_type slot = get_armour_slot(invitem);
    if (item == you.equip[slot] && you.melded[slot])
    {
        mprf("%s is melded into your body!",
             invitem.name(DESC_YOUR).c_str());
        return false;
    }

    if (!item_is_worn(item))
    {
        mpr("You aren't wearing that object!");
        return false;
    }

    // If we get here, we're wearing the item.
    if (invitem.soul_bound())
    {
        mprf("%s is stuck to your body!", invitem.name(DESC_YOUR).c_str());
        return false;
    }

    return true;
}

// TODO: It would be nice if this were made consistent with wear_armour,
// wield_weapon, puton_ring, etc. in terms of taking a default value of -1,
// which has the effect of prompting for an item to take off.
bool takeoff_armour(int item)
{
    if (!_can_takeoff_armour(item))
        return false;

    item_def& invitem = you.inv[item];

    // It's possible to take this thing off, but if it would drop a stat
    // below 0, we should get confirmation.
    if (!_safe_to_remove_or_wear(invitem, true))
        return false;

    you.turn_is_over = true;

    const int delay = armour_equip_delay(invitem);
    start_delay<ArmourOffDelay>(delay - 1, invitem);

    return true;
}

// Returns a list of possible ring slots.
static vector<equipment_type> _current_ring_types()
{
    vector<equipment_type> ret;
    if (you.species == SP_OCTOPODE)
    {
        for (int i = 0; i < 8; ++i)
        {
            const equipment_type slot = (equipment_type)(EQ_RING_ONE + i);

            if (you.get_mutation_level(MUT_MISSING_HAND)
                && slot == EQ_RING_EIGHT)
            {
                continue;
            }

            if (get_form()->slot_available(slot))
                ret.push_back(slot);
        }
    }
    else if (you.species == SP_FAIRY)
    {
        ret.push_back(EQ_FAIRY_JEWEL);
    }
    else
    {
        if (!you.get_mutation_level(MUT_MISSING_HAND))
            ret.push_back(EQ_LEFT_RING);
        ret.push_back(EQ_RIGHT_RING);
    }
    if (you.get_mutation_level(MUT_TENDRILS))
    {
        ret.push_back(EQ_RING_LEFT_TENDRIL);
        ret.push_back(EQ_RING_RIGHT_TENDRIL);
    }
    if (player_equip_unrand(UNRAND_FINGER_AMULET))
        ret.push_back(EQ_RING_AMULET);
    return ret;
}

static vector<equipment_type> _current_jewellery_types()
{
    vector<equipment_type> ret = _current_ring_types();
    if (you.species != SP_FAIRY)
        ret.push_back(EQ_AMULET);
    return ret;
}

static char _ring_slot_key(equipment_type slot)
{
    switch (slot)
    {
    case EQ_LEFT_RING:              return '<';
    case EQ_RIGHT_RING:             return '>';
    case EQ_FAIRY_JEWEL:            return '.';
    case EQ_RING_AMULET:            return '^';
    case EQ_RING_ONE:               return '1';
    case EQ_RING_TWO:               return '2';
    case EQ_RING_THREE:             return '3';
    case EQ_RING_FOUR:              return '4';
    case EQ_RING_FIVE:              return '5';
    case EQ_RING_SIX:               return '6';
    case EQ_RING_SEVEN:             return '7';
    case EQ_RING_EIGHT:             return '8';
    case EQ_RING_LEFT_TENDRIL:      return '(';
    case EQ_RING_RIGHT_TENDRIL:     return ')';
    default:
        die("Invalid ring slot");
    }
}


static int _prompt_ring_to_remove()
{
    const vector<equipment_type> ring_types = _current_ring_types();
    vector<char> slot_chars;
    vector<item_def*> rings;
    for (auto eq : ring_types)
    {
        rings.push_back(you.slot_item(eq, true));
        ASSERT(rings.back());
        slot_chars.push_back(index_to_letter(rings.back()->link));
    }

    if (slot_chars.size() + 2 > msgwin_lines() || ui::has_layout())
    {
        // force a menu rather than a more().
        return EQ_NONE;
    }

    clear_messages();

    mprf(MSGCH_PROMPT,
         "You're wearing all the rings you can. Remove which one?");
    mprf(MSGCH_PROMPT, "(<w>?</w> for menu, <w>Esc</w> to cancel)");

    // FIXME: Needs TOUCH_UI version

    for (size_t i = 0; i < rings.size(); i++)
    {
        string m = "<w>";
        const char key = _ring_slot_key(ring_types[i]);
        m += key;
        if (key == '<')
            m += '<';

        m += "</w> or " + rings[i]->name(DESC_INVENTORY);
        mprf_nocap("%s", m.c_str());
    }
    flush_prev_message();

    // Deactivate choice from tile inventory.
    // FIXME: We need to be able to get the choice (item letter)n
    //        *without* the choice taking action by itself!
    int eqslot = EQ_NONE;

    mouse_control mc(MOUSE_MODE_PROMPT);
    int c;
    do
    {
        c = getchm();
        for (size_t i = 0; i < slot_chars.size(); i++)
        {
            if (c == slot_chars[i]
                || c == _ring_slot_key(ring_types[i]))
            {
                eqslot = ring_types[i];
                c = ' ';
                break;
            }
        }
    } while (!key_is_escape(c) && c != ' ' && c != '?');

    clear_messages();

    if (c == '?')
        return EQ_NONE;
    else if (key_is_escape(c) || eqslot == EQ_NONE)
        return -2;

    return you.equip[eqslot];
}

// Checks whether a to-be-worn or to-be-removed item affects
// character stats and whether wearing/removing it could be fatal.
// If so, warns the player, or just returns false if quiet is true.
static bool _safe_to_remove_or_wear(const item_def &item, bool remove, bool quiet)
{
    if (remove && !safe_to_remove(item, quiet))
        return false;

    int prop_str = 0;
    int prop_dex = 0;
    int prop_int = 0;
    if (item.base_type == OBJ_JEWELLERY)
    {
        switch (item.sub_type)
        {
        case RING_STRENGTH:
            prop_str = 5;
            break;
        case RING_DEXTERITY:
            prop_dex = 5;
            break;
        case RING_INTELLIGENCE:
            prop_int = 5;
            break;
        default:
            break;
        }
    }
    else if (item.base_type == OBJ_ARMOURS && item_type_known(item))
    {
        switch (get_armour_ego_type(item))
        {
        case SPARM_STRENGTH:
            prop_str = 3;
            break;
        case SPARM_INTELLIGENCE:
            prop_int = 3;
            break;
        case SPARM_DEXTERITY:
            prop_dex = 3;
            break;
        default:
            break;
        }
    }

    if (is_artefact(item) || item.cursed())
    {
        prop_str += artefact_property(item, ARTP_STRENGTH);
        prop_int += artefact_property(item, ARTP_INTELLIGENCE);
        prop_dex += artefact_property(item, ARTP_DEXTERITY);
    }

    if (!remove)
    {
        prop_str *= -1;
        prop_int *= -1;
        prop_dex *= -1;
    }

    stat_type red_stat = NUM_STATS;
    if (prop_str >= you.strength() && you.strength() > 0)
        red_stat = STAT_STR;
    else if (prop_int >= you.intel() && you.intel() > 0)
        red_stat = STAT_INT;
    else if (prop_dex >= you.dex() && you.dex() > 0)
        red_stat = STAT_DEX;

    bool disto = false;
    bool vampiric = false;

    if (item.base_type == OBJ_ARMOURS && get_armour_ego_type(item) == SPARM_WIELDING)
    {
        if (you.wearing_ego(EQ_WEAPON0, SPWPN_DISTORTION) || you.wearing_ego(EQ_WEAPON1, SPWPN_DISTORTION)
            && !have_passive(passive_t::safe_distortion))
        {
            disto = true;
        }
        if ((you.wearing_ego(EQ_WEAPON0, SPWPN_VAMPIRISM) || you.wearing_ego(EQ_WEAPON1, SPWPN_VAMPIRISM)
            || (you.weapon(0) && is_unrandom_artefact(*you.weapon(0), UNRAND_MAJIN))
            || (you.weapon(1) && is_unrandom_artefact(*you.weapon(1), UNRAND_MAJIN)))
            && you.undead_state() == US_ALIVE
            && !you_foodless())
        {
            if (you.hunger_state < HS_FULL)
            {
                if (!quiet)
                    mpr("You must be Full or above to do this, lest ye starve!");
                return false;
            }
            vampiric = true;
        }
    }

    if (red_stat == NUM_STATS && !disto && !vampiric)
        return true;

    if (quiet)
        return false;

    string verb = "";
    if (remove)
    {
        if (item.base_type == OBJ_WEAPONS)
            verb = "Unwield";
        else
            verb = "Remov"; // -ing, not a typo
    }
    else
    {
        if (item.base_type == OBJ_WEAPONS)
            verb = "Wield";
        else
            verb = "Wear";
    }

    if (red_stat != NUM_STATS)
    {
        string prompt = make_stringf("%sing this item will reduce your %s to zero "
            "or below. Continue?", verb.c_str(),
            stat_desc(red_stat, SD_NAME));
        if (!yesno(prompt.c_str(), true, 'n', true, false))
        {
            canned_msg(MSG_OK);
            return false;
        }
    }

    if (disto && !remove)
    {
        if (remove)
        {
            string prompt = make_stringf("%sing this item will also remove the protection from distortion."
                " Continue?", verb.c_str());
            if (!yesno(prompt.c_str(), true, 'n', true, false))
            {
                canned_msg(MSG_OK);
                return false;
            }
        }
        else
        {
            string prompt = make_stringf("%sing this item will subject you to wild distortion effects."
                " Continue?", verb.c_str());
            if (!yesno(prompt.c_str(), true, 'n', true, false))
            {
                canned_msg(MSG_OK);
                return false;
            }
        }
    }

    if (vampiric)
    {
        string prompt = make_stringf("%sing this item will hunger you significantly."
            " Continue?", verb.c_str());
        if (!yesno(prompt.c_str(), true, 'n', true, false))
        {
            canned_msg(MSG_OK);
            return false;
        }
    }

    return true;
}

// Checks whether removing an item would cause flight to end and the
// player to fall to their death.
bool safe_to_remove(const item_def &item, bool quiet)
{
    item_info inf = get_item_info(item);

    const bool grants_flight = is_artefact(inf)
            && artefact_property(inf, ARTP_FLY);

    // assumes item can't grant flight twice
    const bool removing_ends_flight = you.airborne()
          && !you.racial_permanent_flight()
          && !you.attribute[ATTR_FLIGHT_UNCANCELLABLE]
          && (you.evokable_flight() == 1);

    const dungeon_feature_type feat = grd(you.pos());

    if (grants_flight && removing_ends_flight
        && is_feat_dangerous(feat, false, true))
    {
        if (!quiet)
            mpr("Losing flight right now would be fatal!");
        return false;
    }

    return true;
}

// Assumptions:
// you.inv[ring_slot] is a valid ring.
// EQ_LEFT_RING and EQ_RIGHT_RING are both occupied, and ring_slot is not
// one of the worn rings.
//
// Does not do amulets.
static bool _swap_rings(int ring_slot)
{
    vector<equipment_type> ring_types = _current_ring_types();
    const int num_rings = ring_types.size();
    int unwanted = 0;
    int last_inscribed = 0;
    int cursed = 0;
    int inscribed = 0;
    int melded = 0; // Both melded rings and unavailable slots.
    int available = 0;
    bool all_same = true;
    item_def* first_ring = nullptr;
    for (auto eq : ring_types)
    {
        item_def* ring = you.slot_item(eq, true);
        if (!you_can_wear(eq, true) || you.melded[eq])
            melded++;
        else if (ring != nullptr)
        {
            if (first_ring == nullptr)
                first_ring = ring;
            else if (all_same)
            {
                if (ring->sub_type != first_ring->sub_type
                    || ring->plus  != first_ring->plus
                    || is_artefact(*ring) || is_artefact(*first_ring))
                {
                    all_same = false;
                }
            }

            if (ring->soul_bound())
                cursed++;
            else if (strstr(ring->inscription.c_str(), "=R"))
            {
                inscribed++;
                last_inscribed = you.equip[eq];
            }
            else
            {
                available++;
                unwanted = you.equip[eq];
            }
        }
    }

    // If the only swappable rings are inscribed =R, go ahead and use them.
    if (available == 0 && inscribed > 0)
    {
        available += inscribed;
        unwanted = last_inscribed;
    }

    // We can't put a ring on, because we're wearing all cursed ones.
    if (melded == num_rings)
    {
        // Shouldn't happen, because hogs and bats can't put on jewellery at
        // all and thus won't get this far.
        mpr("You can't wear that in your present form.");
        return false;
    }
    else if (available == 0)
    {
        mprf("You're already wearing %s cursed ring%s!%s",
             number_in_words(cursed).c_str(),
             (cursed == 1 ? "" : "s"),
             (cursed > 2 ? " Isn't that enough for you?" : ""));
        return false;
    }
    // The simple case - only one available ring.
    // If the jewellery_prompt option is true, always allow choosing the
    // ring slot (even if we still have empty slots).
    else if (available == 1 && !Options.jewellery_prompt)
    {
        if (!remove_ring(unwanted, false))
            return false;
    }
    // We can't put a ring on without swapping - because we found
    // multiple available rings.
    else
    {
        // Don't prompt if all the rings are the same.
        if (!all_same || Options.jewellery_prompt)
            unwanted = _prompt_ring_to_remove();

        if (unwanted == EQ_NONE)
        {
            // do this here rather than in remove_ring so that the custom
            // message is visible.
            unwanted = prompt_invent_item(
                    "You're wearing all the rings you can. Remove which one?",
                    menu_type::invlist, OSEL_UNCURSED_WORN_RINGS, OPER_REMOVE,
                    invprompt_flag::no_warning | invprompt_flag::hide_known);
        }

        // Cancelled:
        if (unwanted < 0)
        {
            canned_msg(MSG_OK);
            return false;
        }

        if (!remove_ring(unwanted, false))
            return false;
    }

    // Put on the new ring.
    start_delay<JewelleryOnDelay>(1, you.inv[ring_slot]);

    return true;
}

static equipment_type _choose_ring_slot()
{
    clear_messages();

    mprf(MSGCH_PROMPT,
         "Put ring on which %s? (<w>Esc</w> to cancel)", you.hand_name(false).c_str());

    const vector<equipment_type> slots = _current_ring_types();
    for (auto eq : slots)
    {
        string msg = "<w>";
        const char key = _ring_slot_key(eq);
        msg += key;
        if (key == '<')
            msg += '<';

        item_def* ring = you.slot_item(eq, true);
        if (ring)
            msg += "</w> or " + ring->name(DESC_INVENTORY);
        else
            msg += "</w> - no ring";

        if (eq == EQ_LEFT_RING)
            msg += " (left)";
        else if (eq == EQ_RIGHT_RING)
            msg += " (right)";
        else if (eq == EQ_FAIRY_JEWEL)
            msg += " (around core)";
        else if (eq == EQ_RING_AMULET)
            msg += " (amulet)";
        mprf_nocap("%s", msg.c_str());
    }
    flush_prev_message();

    equipment_type eqslot = EQ_NONE;
    mouse_control mc(MOUSE_MODE_PROMPT);
    int c;
    do
    {
        c = getchm();
        for (auto eq : slots)
        {
            if (c == _ring_slot_key(eq)
                || (you.slot_item(eq, true)
                    && c == index_to_letter(you.slot_item(eq, true)->link)))
            {
                eqslot = eq;
                c = ' ';
                break;
            }
        }
    } while (!key_is_escape(c) && c != ' ');

    clear_messages();

    return eqslot;
}

// Is it possible to put on the given item in a jewellery slot?
// Preconditions:
// - item_slot is a valid index into inventory
// - item is not already equipped in a jewellery slot
static bool _can_puton_jewellery(int item_slot)
{
    // TODO: between this function, _puton_item, _swap_rings, and remove_ring,
    // there's a bit of duplicated work, and sep. of concerns not clear
    item_def& item = you.inv[item_slot];

    if (item.base_type != OBJ_JEWELLERY)
    {
        mpr("You can only put on jewellery.");
        return false;
    }

    if (you.species == SP_FAIRY)
    {
        int existing = you.equip[EQ_FAIRY_JEWEL];
        if (existing != -1 && you.inv[existing].soul_bound())
        {
            mprf("%s is bound to your soul!",
                you.inv[existing].name(DESC_YOUR).c_str());
            return false;
        }
        return true;
    }

    const bool is_amulet = jewellery_is_amulet(item);

    if (is_amulet && !you_can_wear(EQ_AMULET, true)
        || !is_amulet && !you_can_wear(EQ_RINGS, true))
    {
        mpr("You can't wear that in your present form.");
        return false;
    }

    // Make sure there's at least one slot where we could equip this item
    if (is_amulet)
    {
        int existing = you.equip[EQ_AMULET];
        if (existing != -1 && you.inv[existing].soul_bound())
        {
            mprf("%s is bound to your soul!",
                 you.inv[existing].name(DESC_YOUR).c_str());
            return false;
        }
        else
            return true;
    }
    // The ring case is a bit more complicated
    else
    {
        const vector<equipment_type> slots = _current_ring_types();
        int melded = 0;
        int cursed = 0;
        for (auto eq : slots)
        {
            if (!you_can_wear(eq, true) || you.melded[eq])
            {
                melded++;
                continue;
            }
            int existing = you.equip[eq];
            if (existing != -1 && you.inv[existing].soul_bound())
                cursed++;
            else
                // We found an available slot. We're done.
                return true;
        }
        // If we got this far, there are no available slots.
        if (melded == (int)slots.size())
            mpr("You can't wear that in your present form.");
        else
            mprf("You're already soul bound to %s ring%s!%s",
                 number_in_words(cursed).c_str(),
                 (cursed == 1 ? "" : "s"),
                 (cursed > 2 ? " Isn't that enough for you?" : ""));
        return false;
    }
}

// Put on a particular ring or amulet
static bool _puton_item(int item_slot, bool prompt_slot,
                        bool check_for_inscriptions)
{
    item_def& item = you.inv[item_slot];

    for (int eq = EQ_FIRST_JEWELLERY; eq <= EQ_LAST_JEWELLERY; eq++)
        if (item_slot == you.equip[eq])
        {
            // "Putting on" an equipped item means taking it off.
            if (Options.equip_unequip)
                // TODO: why invert the return value here? failing to remove
                // a ring is equivalent to successfully putting one on?
                return !remove_ring(item_slot);
            else
            {
                mpr("You're already wearing that object!");
                return false;
            }
        }

    if (item_slot == you.equip[EQ_CYTOPLASM])
    {
        if (!yesno("You need to eject it from your cytoplasm first. Continue?", true, 'n'))
            return false;
        if (!eject_item())
            return false;
    }

    if (!_can_puton_jewellery(item_slot))
        return false;

    // It looks to be possible to equip this item. Before going any further,
    // we should prompt the user with any warnings that come with trying to
    // put it on, except when they have already been prompted with them
    // from switching rings.
    if (check_for_inscriptions && !check_warning_inscriptions(item, OPER_PUTON))
    {
        canned_msg(MSG_OK);
        return false;
    }

    const bool is_amulet = jewellery_is_amulet(item);

    const vector<equipment_type> ring_types = _current_ring_types();

    if (you.species == SP_FAIRY)
    {
        if (player_equip_unrand(UNRAND_FINGER_AMULET))
        {
            if (!is_amulet)
            {
                if (!you.slot_item(EQ_RING_AMULET))
                {
                    if (!_safe_to_remove_or_wear(item, false))
                        return false;
                }
                else if (!remove_ring(you.equip[EQ_RING_AMULET], true))
                    return false;
            }
            else
            {
                if (!remove_ring(you.equip[EQ_RING_AMULET], true))
                    return false;

                if (!remove_ring(you.equip[EQ_FAIRY_JEWEL], true))
                    return false;

                if (!_safe_to_remove_or_wear(item, false))
                    return false;

                start_delay<JewelleryOnDelay>(1, item);

                return true;
            }
        }
        else if (you.slot_item(EQ_FAIRY_JEWEL))
        {
            if (!remove_ring(you.equip[EQ_FAIRY_JEWEL], true))
                return false;

            if (!_safe_to_remove_or_wear(item, false))
                return false;

            start_delay<JewelleryOnDelay>(1, item);

            return true;
        }
    }
    else if (!is_amulet)     // i.e. it's a ring
    {
        // Check whether there are any unused ring slots
        bool need_swap = true;
        for (auto eq : ring_types)
        {
            if (!you.slot_item(eq, true))
            {
                need_swap = false;
                break;
            }
        }

        // No unused ring slots. Swap out a worn ring for the new one.
        if (need_swap)
            return _swap_rings(item_slot);
    }
    else if (you.slot_item(EQ_AMULET, true))
    {
        // Remove the previous one.
        if (!remove_ring(you.equip[EQ_AMULET], true))
            return false;

        // Check for stat loss.
        if (!_safe_to_remove_or_wear(item, false))
            return false;

        // Put on the new amulet.
        start_delay<JewelleryOnDelay>(1, item);

        // Assume it's going to succeed.
        return true;
    }
    // At this point, we know there's an empty slot for the ring/amulet we're
    // trying to equip.

    // Check for stat loss.
    if (!_safe_to_remove_or_wear(item, false))
        return false;

    equipment_type hand_used = EQ_NONE;

    if (you.species == SP_FAIRY)
    {
        if (!is_amulet && player_equip_unrand(UNRAND_FINGER_AMULET))
            hand_used = EQ_RING_AMULET;
        else
            hand_used = EQ_FAIRY_JEWEL;
    }
    else if (is_amulet)
        hand_used = EQ_AMULET;
    else if (prompt_slot)
    {
        // Prompt for a slot, even if we have empty ring slots.
        hand_used = _choose_ring_slot();

        if (hand_used == EQ_NONE)
        {
            canned_msg(MSG_OK);
            return false;
        }
        // Allow swapping out a ring.
        else if (you.slot_item(hand_used, true))
        {
            if (!remove_ring(you.equip[hand_used], false))
                return false;

            start_delay<JewelleryOnDelay>(1, item);
            return true;
        }
    }
    else
    {
        for (auto eq : ring_types)
        {
            if (!you.slot_item(eq, true))
            {
                hand_used = eq;
                break;
            }
        }
    }

    const unsigned int old_talents = your_talents(false).size();

    // Actually equip the item.
    equip_item(hand_used, item_slot);

    check_item_hint(you.inv[item_slot], old_talents);
#ifdef USE_TILE_LOCAL
    if (your_talents(false).size() != old_talents)
    {
        tiles.layout_statcol();
        redraw_screen();
    }
#endif

    // Putting on jewellery is fast.
    you.time_taken /= 2;
    you.turn_is_over = true;

    return true;
}

// Ejection delay is half of Subsumption delay; rounded up.
static int _subsumption_delay(const item_def & item)
{
    switch (item.base_type)
    {
    case OBJ_ARMOURS:
        return armour_equip_delay(item) * 1.5;
    case OBJ_JEWELLERY:
        return 2;
    case OBJ_MISCELLANY: // Only the Lantern of Shadows
        return 3;
    case OBJ_STAVES:
    case OBJ_WEAPONS:
    case OBJ_SHIELDS:
        return      !can_wield(&item, false, true) ? 7 :
               (you.hands_reqd(item) == HANDS_TWO) ? 5
                                                   : 3;
    default: // Nothing should get here.
        mpr("Unhandled Subsumption Delay (Please report!). Defaulting to 3 turns.");
        return 3;
    }
}

bool subsume_item(int slot)
{
    if (!you.get_mutation_level(MUT_CYTOPLASMIC_SUSPENSION))
    {
        mpr("You can't subsume anything.");
        return false;
    }

    if (inv_count() < 1)
    {
        canned_msg(MSG_NOTHING_CARRIED);
        return false;
    }

    if (you.berserk())
    {
        canned_msg(MSG_TOO_BERSERK);
        return false;
    }

    const int equipn =
        (slot == -1) ? prompt_invent_item("Subsume which item?",
            menu_type::invlist,
            OSEL_SUBSUMABLE,
            OPER_WEAR,
            invprompt_flag::no_warning
            | invprompt_flag::hide_known)
        : slot;

    if (prompt_failed(equipn))
        return false;

    item_def &item = you.inv[equipn];

    if (!item_is_subsumable(item))
    {
        if (item.link == you.equip[EQ_CYTOPLASM])
            mpr("You've already subsumed that!");
        else
            mpr("You can't subsume that!");
        return false;
    }

    if (item_is_equipped(item))
    {
        switch (item.base_type)
        {
        case OBJ_ARMOURS: // Removing armour is slow so prompt...
            if (!yesno("You have to take that off in order to subsume it. Continue?", true, 'n'))
            {
                canned_msg(MSG_OK);
                return false;
            }
            takeoff_armour(item.link);
            break;
        case OBJ_JEWELLERY: // Rest are fast so no prompt.
            remove_ring(item.link);
            break;
        case OBJ_SHIELDS:
        case OBJ_STAVES:
        case OBJ_WEAPONS:
        case OBJ_MISCELLANY: // Lantern of Shadows.
            unwield_item(item.link == you.equip[EQ_WEAPON0]);
            break;
        default:
            mprf(MSGCH_ERROR, "Unhandled item type.");
            return false;
        }
    }

    if (item.base_type == OBJ_STAVES)
    {
        const bool hand0 = you.weapon(0) && you.weapon(0)->base_type == OBJ_STAVES;
        const bool hand1 = you.weapon(1) && you.weapon(1)->base_type == OBJ_STAVES;

        if (hand0 || hand1)
        {
            if (!yesno("You can only attune one staff at a time, unwield your current one?", true, 'n'))
            {
                canned_msg(MSG_OK);
                return false;
            }
            unwield_item(hand0);
        }
    }

    bool lopen = false;
    if (needs_handle_warning(item, OPER_SUBSUME, lopen))
    {
        string prompt =
            "Really subsume " + item.name(DESC_INVENTORY) + "?";
        if (lopen)
            prompt += " This could place you under penance!";

        if (!yesno(prompt.c_str(), false, 'n'))
        {
            canned_msg(MSG_OK);
            return false;
        }
    }

    if (you.equip[EQ_CYTOPLASM] != -1)
        eject_item();

    int delay = _subsumption_delay(item);
    start_delay<SubsumptionDelay>(delay, item);

    return true;
}

bool eject_item()
{
    if (!you.get_mutation_level(MUT_CYTOPLASMIC_SUSPENSION))
    {
        mpr("You don't have a cytoplasmic pocket to eject from.");
        return false;
    }

    if (you.equip[EQ_CYTOPLASM] == -1)
    {
        mpr("Nothing to eject.");
        return false;
    }

    if (you.berserk())
    {
        canned_msg(MSG_TOO_BERSERK);
        return false;
    }

    item_def * remove = you.slot_item(EQ_CYTOPLASM);
    bool lopen = false;

    if (needs_handle_warning(*remove, OPER_EJECT, lopen))
    {
        string prompt =
            "Really eject " + remove->name(DESC_INVENTORY) + "?";
        if (lopen)
            prompt += " This could place you under penance!";

        if (!yesno(prompt.c_str(), false, 'n'))
        {
            canned_msg(MSG_OK);
            return false;
        }
    }

    item_def &item = *you.slot_item(EQ_CYTOPLASM);
    int delay = (div_round_up(_subsumption_delay(item), 2));

    start_delay<EjectionDelay>(delay, item);

    return true;
}

// Put on a ring or amulet. (If slot is -1, first prompt for which item to put on)
bool puton_ring(int slot, bool allow_prompt, bool check_for_inscriptions)
{
    int item_slot;

    if (inv_count() < 1)
    {
        canned_msg(MSG_NOTHING_CARRIED);
        return false;
    }

    if (you.berserk())
    {
        canned_msg(MSG_TOO_BERSERK);
        return false;
    }

    if (slot != -1)
        item_slot = slot;
    else
    {
        item_slot = prompt_invent_item("Put on which piece of jewellery?",
                                       menu_type::invlist, OBJ_JEWELLERY,
                                       OPER_PUTON, invprompt_flag::no_warning);
    }

    if (prompt_failed(item_slot))
        return false;

    bool prompt = allow_prompt ? Options.jewellery_prompt : false;

    return _puton_item(item_slot, prompt, check_for_inscriptions);
}

// Remove the amulet/ring at given inventory slot (or, if slot is -1, prompt
// for which piece of jewellery to remove)
bool remove_ring(int slot, bool announce)
{
    equipment_type hand_used = EQ_NONE;
    int ring_wear_2;
    bool has_jewellery = false;
    bool has_melded = false;
    const vector<equipment_type> jewellery_slots = _current_jewellery_types();

    for (auto eq : jewellery_slots)
    {
        if (you.slot_item(eq))
        {
            if (has_jewellery || Options.jewellery_prompt)
            {
                // At least one other piece, which means we'll have to ask
                hand_used = EQ_NONE;
            }
            else
                hand_used = eq;

            has_jewellery = true;
        }
        else if (you.melded[eq])
            has_melded = true;
    }

    if (!has_jewellery)
    {
        if (has_melded)
            mpr("You aren't wearing any unmelded rings or amulets.");
        else
            mpr("You aren't wearing any rings or amulets.");

        return false;
    }

    if (you.berserk())
    {
        canned_msg(MSG_TOO_BERSERK);
        return false;
    }

    // If more than one equipment slot had jewellery, we need to figure out
    // which one to remove from.
    if (hand_used == EQ_NONE)
    {
        const int equipn =
            (slot == -1)? prompt_invent_item("Remove which piece of jewellery?",
                                             menu_type::invlist,
                                             OBJ_JEWELLERY,
                                             OPER_REMOVE,
                                             invprompt_flag::no_warning
                                                | invprompt_flag::hide_known)
                        : slot;

        if (prompt_failed(equipn))
            return false;

        hand_used = item_equip_slot(you.inv[equipn]);
        if (hand_used == EQ_NONE)
        {
            mpr("You aren't wearing that.");
            return false;
        }
        else if (you.inv[equipn].base_type != OBJ_JEWELLERY)
        {
            mpr("That isn't a piece of jewellery.");
            return false;
        }
    }

    if (you.equip[hand_used] == -1)
    {
        mpr("I don't think you really meant that.");
        return false;
    }
    else if (you.melded[hand_used])
    {
        mpr("You can't take that off while it's melded.");
        return false;
    }
    else if ((hand_used == EQ_AMULET || hand_used == EQ_FAIRY_JEWEL)
        && you.equip[EQ_RING_AMULET] != -1)
    {
        // This can be removed in the future if more ring amulets are added.
        ASSERT(player_equip_unrand(UNRAND_FINGER_AMULET));

        remove_ring(you.equip[EQ_RING_AMULET], true);
    }

    if (!check_warning_inscriptions(you.inv[you.equip[hand_used]],
                                    OPER_REMOVE))
    {
        canned_msg(MSG_OK);
        return false;
    }

    if (you.inv[you.equip[hand_used]].soul_bound())
    {
        if (announce)
        {
            mprf("%s is bound to to your soul!",
                 you.inv[you.equip[hand_used]].name(DESC_YOUR).c_str());
        }
        else
            mpr("It's bound to your soul!");

        set_ident_flags(you.inv[you.equip[hand_used]], ISFLAG_KNOW_CURSE);
        return false;
    }

    ring_wear_2 = you.equip[hand_used];

    // Remove the ring.
    if (!_safe_to_remove_or_wear(you.inv[ring_wear_2], true))
        return false;

#ifdef USE_SOUND
    parse_sound(REMOVE_JEWELLERY_SOUND);
#endif
    mprf("You remove %s.", you.inv[ring_wear_2].name(DESC_YOUR).c_str());
#ifdef USE_TILE_LOCAL
    const unsigned int old_talents = your_talents(false).size();
#endif
    unequip_item(hand_used);
#ifdef USE_TILE_LOCAL
    if (your_talents(false).size() != old_talents)
    {
        tiles.layout_statcol();
        redraw_screen();
    }
#endif

    you.time_taken /= 2;
    you.turn_is_over = true;

    return true;
}

void prompt_inscribe_item()
{
    if (inv_count() < 1)
    {
        mpr("You don't have anything to inscribe.");
        return;
    }

    int item_slot = prompt_invent_item("Inscribe which item?",
                                       menu_type::invlist, OSEL_ANY);

    if (prompt_failed(item_slot))
        return;

    inscribe_item(you.inv[item_slot]);
}

static bool _check_blood_corpses_on_ground()
{
    for (stack_iterator si(you.pos(), true); si; ++si)
    {
        if (si->is_type(OBJ_CORPSES, CORPSE_BODY)
            && mons_has_blood(si->mon_type))
        {
            return true;
        }
    }
    return false;
}

static void _vampire_corpse_help()
{
    if (you.species != SP_VAMPIRE)
        return;

    if (_check_blood_corpses_on_ground())
        mpr("Use <w>e</w> to drain blood from corpses.");
}

void drink(item_def* potion)
{
    if (you.undead_state() == US_UNDEAD)
    {
        mpr("You can't drink.");
        return;
    }

    if (you.duration[DUR_NO_POTIONS])
    {
        if (you.species == SP_LIGNIFITE)
            mpr("You cannot absorb potions through your roots in your current state!");
        else
            mpr("You cannot drink potions in your current state!");
        return;
    }

    if (!potion)
    {
        if (you.species == SP_LIGNIFITE)
            potion = use_an_item(OBJ_POTIONS, OPER_QUAFF, "Pour which item on your roots?");
        else 
            potion = use_an_item(OBJ_POTIONS, OPER_QUAFF, "Drink which item?");

        if (!potion)
        {
            _vampire_corpse_help();
            return;
        }
    }

    if (potion->base_type != OBJ_POTIONS)
    {
        if (you.species == SP_LIGNIFITE)
            mpr("You can't absorb that!");
        else
            mpr("You can't drink that!");
        return;
    }

    const bool alreadyknown = item_type_known(*potion);

    if (alreadyknown && is_bad_item(*potion, true))
    {
        canned_msg(MSG_UNTHINKING_ACT);
        return;
    }

    bool penance = god_hates_item(*potion);
    bool resistant = you.rmut_from_item() && (potion->sub_type == POT_MUTATION);
    string prompt = make_stringf("Really quaff the %s%s",
                                 potion->name(DESC_DBNAME).c_str(),
                                 penance ? "? This action would place"
                                           " you under penance!" 
                             : resistant ? ", while resistant to mutation?"
                                         : "?");
    if (alreadyknown && (is_dangerous_item(*potion, true) || penance)
        && Options.bad_item_prompt
        && !yesno(prompt.c_str(), false, 'n'))
    {
        canned_msg(MSG_OK);
        return;
    }

    // The "> 1" part is to reduce the amount of times that Xom is
    // stimulated when you are a low-level 1 trying your first unknown
    // potions on monsters.
    const bool dangerous = (player_in_a_dangerous_place()
                            && you.experience_level > 1);

    if (player_under_penance(GOD_GOZAG) && one_chance_in(3))
    {
        simple_god_message(" petitions for your drink to fail.", GOD_GOZAG);
        you.turn_is_over = true;
        return;
    }

    if (!quaff_potion(*potion))
        return;

    if (!alreadyknown && dangerous)
    {
        // Xom loves it when you drink an unknown potion and there is
        // a dangerous monster nearby...
        xom_is_stimulated(200);
    }

    // We'll need this later, after destroying the item.
    const bool was_exp = potion->sub_type == POT_EXPERIENCE;
    if (in_inventory(*potion))
    {
        dec_inv_item_quantity(potion->link, 1);
        auto_assign_item_slot(*potion);
    }
    else
        dec_mitm_item_quantity(potion->index(), 1);

    item_def * bottles = nullptr;

    for (auto &item : you.inv)
    {
        if (item.defined() && item.is_type(OBJ_MISCELLANY, MISC_EMPTY_BOTTLE))
            bottles = &item;
    }

    if (bottles)
        bottles->quantity++;
    else
    {
        int bint = items(false, OBJ_MISCELLANY, MISC_EMPTY_BOTTLE, 1);
        if (bint != NON_ITEM)
        {
            mpr("You drop the empty bottle.");
            move_item_to_grid(&bint, you.pos());
        }
    }
    
    count_action(CACT_USE, OBJ_POTIONS);
    you.turn_is_over = true;

    // This got deferred from PotionExperience::effect to prevent SIGHUP abuse.
    if (was_exp)
        level_change();
}

// XXX: there's probably a nicer way of doing this.
bool god_hates_brand(const int brand)
{
    if (is_good_god(you.religion)
        && (brand == SPWPN_DRAINING
            || brand == SPWPN_VAMPIRISM
            || brand == SPWPN_CHAOS
            || brand == SPWPN_PAIN))
    {
        return true;
    }

    if (you_worship(GOD_CHEIBRIADOS) && (brand == SPWPN_CHAOS
                                         || brand == SPWPN_SPEED))
    {
        return true;
    }

    if (you_worship(GOD_YREDELEMNUL) && brand == SPWPN_HOLY_WRATH)
        return true;

    if (you_worship(GOD_JIYVA) && brand == SPWPN_SILVER)
        return true;

    return false;
}

static void _rebrand_staff(item_def &stf)
{
    const facet_type old_brand = get_staff_facet(stf);
    facet_type new_brand = old_brand;

    while (old_brand == new_brand || new_brand == SPSTF_NORMAL)
        new_brand = generate_staff_facet(stf, 500);

    stf.brand = new_brand;
}

static void _rebrand_armour(item_def &arm)
{
    const special_armour_type old_brand = get_armour_ego_type(arm);
    special_armour_type new_brand = old_brand;

    while (old_brand == new_brand)
    {
        new_brand = generate_armour_type_ego((armour_type)arm.sub_type);
        if (one_chance_in(3) || new_brand == SPARM_NORMAL)
            new_brand = random_choose(SPARM_FIRE_RESISTANCE, SPARM_COLD_RESISTANCE, SPARM_STRENGTH, SPARM_DEXTERITY);
    }

    arm.brand = new_brand;
}

static void _rebrand_shield(item_def &shd)
{
    const special_armour_type old_brand = get_armour_ego_type(shd);
    special_armour_type new_brand = old_brand;

    while (old_brand == new_brand)
        new_brand = defensive_shield_brand();

    shd.brand = new_brand;
}

static void _rebrand_weapon(item_def& wpn)
{
    if (you.duration[DUR_EXCRUCIATING_WOUNDS]
        && (wpn.link == you.props[PAINED_WEAPON_KEY].get_short()))
    {
        end_weapon_brand();
    }
    const brand_type old_brand = get_weapon_brand(wpn);
    brand_type new_brand = old_brand;

    // now try and find an appropriate brand
    while (old_brand == new_brand || god_hates_brand(new_brand))
    {
        if (is_range_weapon(wpn))
        {
            new_brand = random_choose_weighted(
                                    33, SPWPN_MOLTEN,
                                    33, SPWPN_FREEZING,
                                    23, SPWPN_VENOM,
                                    23, SPWPN_VORPAL,
                                     5, SPWPN_ELECTROCUTION,
                                     3, SPWPN_CHAOS);
        }
        else
        {
            new_brand = random_choose_weighted(
                                    30, SPWPN_MOLTEN,
                                    30, SPWPN_FREEZING,
                                    25, SPWPN_VORPAL,
                                    20, SPWPN_VENOM,
                                    15, SPWPN_ACID,
                                    15, SPWPN_ELECTROCUTION,
                                    12, SPWPN_PROTECTION,
                                     8, SPWPN_VAMPIRISM,
                                     3, SPWPN_CHAOS);
        }
    }

    wpn.brand = new_brand;
    convert2bad(wpn);
}

static string _item_name(item_def &item)
{
    return item.name(in_inventory(item) ? DESC_YOUR : DESC_THE);
}

static void _finish_rebranding(item_def &item, colour_t flash_colour, bool player = true)
{
    item_set_appearance(item);
    // Message would spoil this even if we didn't identify.
    set_ident_flags(item, ISFLAG_KNOW_TYPE);
    if (player)
    {
        mprf_nocap("%s", item.name(DESC_INVENTORY_EQUIP).c_str());
        you.wield_change = true;
        you.redraw_armour_class = true;
        you.redraw_evasion = true;
        you.redraw_resists = true;
    }
    flash_view_delay(player ? UA_PLAYER : UA_MONSTER, flash_colour, 300);
}

static void _brand_staff(item_def &stf)
{
    const string itname = _item_name(stf);

    _rebrand_staff(stf);

    colour_t flash_colour = BLACK;

    switch (get_staff_facet(stf))
    {
    case SPSTF_ACCURACY:
        flash_colour = WHITE;
        mprf("%s feels like an instrument of precision!", itname.c_str());
        break;

    case SPSTF_CHAOS:
        flash_colour = ETC_CHAOS;
        mprf("%s exudes an aura of scintillating colours!", itname.c_str());
        break;

    case SPSTF_ENERGY:
        flash_colour = ETC_MAGIC;
        mprf("%s makes you feel much more energetic!", itname.c_str());
        break;

    case SPSTF_FLAY:
        flash_colour = ETC_BLOOD;
        mprf("%s gives off an evil aura!", itname.c_str());
        break;

    case SPSTF_MENACE:
        flash_colour = ETC_DARK;
        mprf("%s feels corruptingly powerful!", itname.c_str());
        break;

    case SPSTF_REAVER:
        flash_colour = LIGHTGRAY;
        mprf("Even touching %s makes you feel more able to cast in armour!", itname.c_str());
        break;

    case SPSTF_SCOPED:
        flash_colour = CYAN;
        mprf("%s extends the range of your destructive magicks!", itname.c_str());
        break;

    case SPSTF_SHIELD:
        flash_colour = YELLOW;
        mprf("%s exudes a shield of force!", itname.c_str());
        break;

    case SPSTF_WARP:
        flash_colour = ETC_WARP;
        mprf("%s twists and distorts painfully!", itname.c_str());
        break;

    case SPSTF_WIZARD:
        flash_colour = LIGHTMAGENTA;
        mprf("%s increases your casting ability!", itname.c_str());
        break;

    default:
        break;
    }

    _finish_rebranding(stf, flash_colour);
}

static void _brand_armour(item_def &arm)
{
    const string itname = _item_name(arm);

    if (arm.base_type == OBJ_SHIELDS)
        _rebrand_shield(arm);
    else
        _rebrand_armour(arm);

    colour_t flash_colour = BLACK;

    bool plural = (arm.sub_type == ARM_BOOTS || arm.sub_type == ARM_GLOVES || arm.sub_type == ARM_CLAW ||
        (armour_is_hide(arm) && arm.sub_type != ARM_DEEP_TROLL_LEATHER_ARMOUR 
                             && arm.sub_type != ARM_IRON_TROLL_LEATHER_ARMOUR && arm.sub_type != ARM_SALAMANDER_HIDE_ARMOUR));

    switch (get_armour_ego_type(arm))
    {
    default:
    case SPARM_DEXTERITY:
    case SPARM_INTELLIGENCE:
    case SPARM_STRENGTH:
        flash_colour = YELLOW;
        mprf("%s emit%s a brilliant flash of light!", itname.c_str(), plural ? "" : "s");
        break;

    case SPARM_ARCHERY:
        flash_colour = ETC_WU_JIAN;
        mprf("%s make%s you feel more skilled with ranged weapons!", itname.c_str(), plural ? "" : "s");
        break;

    case SPARM_WIELDING:
        flash_colour = ETC_WU_JIAN;
        mprf("%s make%s you feel more skilled with melee weapons!", itname.c_str(), plural ? "" : "s");
        break;

    case SPARM_ARCHMAGI:
        flash_colour = ETC_MAGIC;
        mprf("%s exude%s a strong magical aura!", itname.c_str(), plural ? "" : "s");
        break;

    case SPARM_CLOUD_IMMUNE:
        flash_colour = ETC_AIR;
        mprf("%s seem%s to repel mist in the air!", itname.c_str(), plural ? "" : "s");
        break;

    case SPARM_COLD_RESISTANCE:
        flash_colour = LIGHTCYAN;
        mprf("%s begin%s to feel comfortably warm!", itname.c_str(), plural ? "" : "s");
        break;

    case SPARM_FIRE_RESISTANCE:
        flash_colour = RED;
        mprf("%s begin%s to feel soothingly cool!", itname.c_str(), plural ? "" : "s");
        break;

    case SPARM_HIGH_PRIEST:
    {
        bool evil = is_evil_god(you.religion);
        flash_colour = evil ? ETC_UNHOLY : ETC_HOLY;
        mprf("%s begin%s to exude a%sholy aura!", itname.c_str(), plural ? "" : "s", evil ? "n un" : " ");
    }
        break;

    case SPARM_IMPROVED_VISION:
        flash_colour = WHITE;
        mprf("%s begin%s to peer through the darkness!", itname.c_str(), plural ? "" : "s");
        break;

    case SPARM_MAGIC_RESISTANCE:
        flash_colour = ETC_MAGIC;
        mprf("%s begin%s to repel magical effects!", itname.c_str(), plural ? "" : "s");
        break;
        
    case SPARM_POISON_RESISTANCE:
        flash_colour = LIGHTGREEN;
        mprf("%s become%s much healthier!", itname.c_str(), plural ? "" : "s");
        break;

    case SPARM_PONDEROUSNESS:
        flash_colour = LIGHTGRAY;
        mprf("%s feel%s sluggish to the touch!", itname.c_str(), plural ? "" : "s");
        break;

    case SPARM_POSITIVE_ENERGY:
        flash_colour = WHITE;
        mprf("%s begin%s to feel very comfortable!", itname.c_str(), plural ? "" : "s");
        break;

    case SPARM_PROTECTION:
        flash_colour = YELLOW;
        mprf("%s project%s a defensive aura!", itname.c_str(), plural ? "" : "s");
        break;

    case SPARM_REFLECTION:
        flash_colour = ETC_SILVER;
        mprf("%s project%s an invisible shield of force!", itname.c_str(), plural ? "" : "s");
        break;

    case SPARM_REPULSION:
        flash_colour = BLUE;
        mprf("%s repel%s away your touch!", itname.c_str(), plural ? "" : "s");
        break;

    case SPARM_RESISTANCE:
        flash_colour = LIGHTRED;
        mprf("%s maintain%s a comfortable temperature!", itname.c_str(), plural ? "" : "s");
        break;

    case SPARM_RUNNING:
        flash_colour = ETC_SILVER;
        mprf("%s make%s you feel like running away!", itname.c_str(), plural ? "" : "s");
        break;

    case SPARM_SPIRIT_SHIELD:
        flash_colour = ETC_MUTAGENIC;
        mprf("%s well%s up with protective spirits!", itname.c_str(), plural ? "" : "s");
        break;

    case SPARM_INVISIBILITY:
    case SPARM_STEALTH:
        flash_colour = DARKGREY;
        mprf("%s sink%s into the shadows!", itname.c_str(), plural ? "" : "s");
        break;

    case SPARM_SOFT:
        flash_colour = WHITE;
        mprf("%s feel%s extremely comfortably soft!", itname.c_str(), plural ? "" : "s");
        break;

    case SPARM_INSULATION:
        flash_colour = CYAN;
        mprf("%s feel%s insulated against electric shocks!", itname.c_str(), plural ? "" : "s");
        break;

    case SPARM_STURDY:
        flash_colour = BROWN;
        mprf("%s feel%s very rugged!", itname.c_str(), plural ? "" : "s");
        break;
    }

    _finish_rebranding(arm, flash_colour);
}

void brand_weapon(item_def &wpn, bool player)
{
    const string itname = _item_name(wpn);

    _rebrand_weapon(wpn);

    colour_t flash_colour = BLACK;

    switch (get_weapon_brand(wpn))
    {
    case SPWPN_VORPAL:
        flash_colour = YELLOW;
        mprf("%s emits a brilliant flash of light!",itname.c_str());
        break;

    case SPWPN_PROTECTION:
        flash_colour = YELLOW;
        mprf("%s projects an invisible shield of force!",itname.c_str());
        break;

    case SPWPN_MOLTEN:
        flash_colour = RED;
        mprf("%s melts into a flaming hot liquid!", itname.c_str());
        break;

    case SPWPN_FREEZING:
        flash_colour = LIGHTCYAN;
        mprf("%s is covered with a thin layer of ice!", itname.c_str());
        break;

    case SPWPN_DRAINING:
        flash_colour = DARKGREY;
        mprf("%s craves living souls!", itname.c_str());
        break;

    case SPWPN_VAMPIRISM:
        flash_colour = DARKGREY;
        mprf("%s thirsts for the lives of mortals!", itname.c_str());
        break;

    case SPWPN_VENOM:
        flash_colour = GREEN;
        mprf("%s drips with poison.", itname.c_str());
        break;

    case SPWPN_ELECTROCUTION:
        flash_colour = LIGHTCYAN;
        mprf("%s crackles with electricity.", itname.c_str());
        break;

    case SPWPN_CHAOS:
        flash_colour = ETC_JEWEL;
        mprf("%s erupts in a glittering mayhem of colour.", itname.c_str());
        break;

    case SPWPN_ACID:
        flash_colour = ETC_SLIME;
        mprf("%s oozes corrosive slime.", itname.c_str());
        break;

    default:
        mpr("Scroll failed. This is a bug.");
        break;
    }

    _finish_rebranding(wpn, flash_colour, player);

    return;
}

static item_def* _choose_target_item_for_scroll(bool scroll_known, object_selector selector,
                                                const char* prompt)
{
    return use_an_item(selector, OPER_ANY, prompt,
                       [=]()
                       {
                           if (scroll_known
                               || crawl_state.seen_hups
                               || yesno("Really abort (and waste the scroll)?", false, 0))
                           {
                               return true;
                           }
                           return false;
                       });
}

// Returns true if the scroll is used up.
static bool _handle_bless_item(const string &pre_msg)
{
    item_def* item = _choose_target_item_for_scroll(true, OSEL_BLESSABLE_ITEM, "Bless which item?");

    if (!item)
        return false;

    mpr(pre_msg);

    if (item->cursed())
    {
        bool uncurse = true;
        if (you_worship(GOD_ASHENZARI) && !is_artefact(*item) &&
            (item->base_type == OBJ_WEAPONS || item->base_type == OBJ_ARMOURS ||
             item->base_type == OBJ_SHIELDS || item->base_type == OBJ_STAVES))
        {
            mprf(MSGCH_GOD, "Ashenzari asks: Mortal, do you wish for us to preserve the curse?");
            uncurse = !yesno("Preserve curse?", true, 0);
        }
        if (uncurse)
        {
            do_uncurse_item(*item);
            mprf("The blessing and the curse cancel each other in a brilliant flash!");
            flash_view_delay(UA_PLAYER, WHITE, 300);
            return true;
        }
    }

    switch (item->base_type)
    {
    case OBJ_WEAPONS:
        brand_weapon(*item);
        break;

    case OBJ_ARMOURS:
        if (item->sub_type == ARM_CLAW)
            brand_weapon(*item);
        else
            _brand_armour(*item);
        break;

    case OBJ_SHIELDS:
        if (is_hybrid(item->sub_type))
            brand_weapon(*item, true);
        else
            _brand_armour(*item);
        break;

    case OBJ_STAVES:
        _brand_staff(*item);
        break;

    default:
        mpr("Bad item selected for scroll. Please file a bug report.");
        break;
    }
    return true;
}

bool enchant_item(item_def &item, bool quiet, bool player)
{
    ASSERT(item.defined());

    if (!is_enchantable_item(item))
    {
        if (!quiet)
            canned_msg(MSG_NOTHING_HAPPENS);
        return false;
    }

    // Get item name now before changing enchantment.
    string iname = _item_name(item);

    colour_t flash_colour = BLACK;

    if (((item.base_type == OBJ_ARMOURS && item.sub_type != ARM_CLAW) || (item.base_type == OBJ_SHIELDS && !is_hybrid(item.sub_type))))
    {
        if (!quiet)
        {
            const bool plural = armour_is_hide(item)
                && item.sub_type != ARM_TROLL_LEATHER_ARMOUR
                && item.sub_type != ARM_IRON_TROLL_LEATHER_ARMOUR
                && item.sub_type != ARM_DEEP_TROLL_LEATHER_ARMOUR;
            mprf("%s %s green for a moment.",
                iname.c_str(),
                conjugate_verb("glow", plural).c_str());
        }
        flash_colour = GREEN;
        if (player)
            you.redraw_armour_class = true;
    }

    if (item.base_type == OBJ_WEAPONS || (item.base_type == OBJ_SHIELDS && is_hybrid(item.sub_type) || item.is_type(OBJ_ARMOURS, ARM_CLAW)))
    {
        flash_colour = RED;
        if (!quiet)
            mprf("%s glows red for a moment.", iname.c_str());
    }

    if (item.base_type == OBJ_STAVES)
    {
        flash_colour = LIGHTMAGENTA;
        if (!quiet)
            mprf("%s glows fuchsia for a moment.", iname.c_str());
    }

    if (item.base_type == OBJ_WANDS)
    {
        if (!quiet)
            mprf("%s glows gold for a moment.", iname.c_str());

        flash_colour = YELLOW;
        item.charges += 1 + random2avg(wand_charge_value(item.sub_type), 3);
        return true;
    }
    
    if (player)
        you.wield_change = true;
    
    flash_view_delay(player ? UA_PLAYER : UA_MONSTER, flash_colour, 300);

    item.plus++;

    return true;
}

static int _handle_enchant_item(bool alreadyknown, const string &pre_msg)
{
    item_def* target = _choose_target_item_for_scroll(alreadyknown, OSEL_ENCHANTABLE_ITEM,
                                                      "Enchant which item?");

    if (!target)
        return alreadyknown ? -1 : 0;

    // Okay, we may actually (attempt to) enchant something.
    if (alreadyknown)
        mpr(pre_msg);

    bool result = enchant_item(*target, false);

    return result ? 1 : 0;
}

void random_uselessness(actor * act)
{
    ASSERT(!crawl_state.game_is_arena());

    switch (random2(8))
    {
    case 0:
    case 1:
        mprf("The dust glows %s!", weird_glowing_colour().c_str());
        break;

    case 2:
        if (act->weapon())
        {
            mprf("%s%s%s glows %s for a moment.",
                act->is_player() ? "" : act->name(DESC_ITS).c_str(),
                act->is_player() ? "" : " ",
                act->weapon()->name(act->is_player() ? DESC_YOUR
                                                     : DESC_PLAIN).c_str(),
                weird_glowing_colour().c_str());
        }
        else if (act->is_player())
        {
            mpr(you.hands_act("glow", weird_glowing_colour()
                                      + " for a moment."));
        }
        else
        {
            mprf("%s flashes %s for a moment.", act->name(DESC_THE).c_str(), 
                weird_glowing_colour().c_str());
        }
        break;

    case 3:
        if (act->is_player() && you.char_class == JOB_MUMMY && you.undead_state() == US_UNDEAD)
            mpr("Your bandages flutter.");
        else if (you.can_smell())
            mprf("You smell %s.", _weird_smell().c_str());
        else
            mpr("The air gets thick around you.");
        break;

    case 4:
        if (act->is_player())
            mpr("You experience a momentary feeling of inescapable doom!");
        else
            simple_monster_message(*act->as_monster(), " looks extremely uncomfortable for a moment.");
        break;

    case 5:
        if (you.get_mutation_level(MUT_BEAK) || one_chance_in(3))
            mpr("Your brain hurts!");
        else if (you.undead_state() == US_UNDEAD || coinflip())
            mpr("Your ears itch!");
        else
            mpr("Your nose twitches suddenly!");
        break;

    case 6:
    {
        if (!silenced(you.pos()))
            mprf(MSGCH_SOUND, "You hear the tinkle of a tiny bell.");
        else
            mpr("Butterflies seem to form from the dust.");
        noisy(2, act->pos());
        int duration = 20 + random2(10);
        int amt = roll_dice(3, 4);
        for (int i = 0; i < amt; ++i)
        {
            create_monster(
                mgen_data(MONS_BUTTERFLY, BEH_STRICT_NEUTRAL,
                    act->pos(), MHITNOT)
                .set_summoned(act, duration, 0));
        }
    }
        break;
    case 7:
        if (!silenced(you.pos()))
            mprf(MSGCH_SOUND, "You hear %s.", _weird_sound().c_str());
        else
            mpr("Nothing appears to happen.");
        noisy(6, act->pos());
        break;
    }
}

static void _handle_read_book(item_def& book)
{
    if (you.berserk())
    {
        canned_msg(MSG_TOO_BERSERK);
        return;
    }

    if (you.duration[DUR_BRAINLESS])
    {
         canned_msg(MSG_BRAINLESS);
        return;
    }

    ASSERT(book.sub_type != BOOK_MANUAL);

#if TAG_MAJOR_VERSION == 34
    if (book.sub_type == BOOK_BUGGY_DESTRUCTION)
    {
        mpr("This item has been removed, sorry!");
        return;
    }
#endif

    set_ident_flags(book, ISFLAG_IDENT_MASK);
    read_book(book);
}

static void _vulnerability_scroll()
{
    mon_enchant lowered_mr(ENCH_LOWERED_MR, 1, &you, 400);

    // Go over all creatures in LOS.
    for (radius_iterator ri(you.pos(), LOS_NO_TRANS); ri; ++ri)
    {
        if (monster* mon = monster_at(*ri))
        {
            // If relevant, monsters have their MR halved.
            if (!mons_immune_magic(*mon))
                mon->add_ench(lowered_mr);

            // Annoying but not enough to turn friendlies against you.
            if (!mon->wont_attack())
                behaviour_event(mon, ME_ANNOY, &you);
        }
    }

    you.set_duration(DUR_LOWERED_MR, 40, 0, "Magic quickly surges around you.");
    you.redraw_resists = true;
}

static bool _is_cancellable_scroll(scroll_type scroll)
{
    return    scroll == SCR_BLINKING
           || scroll == SCR_ENCHANT
#if TAG_MAJOR_VERSION == 34
           || scroll == SCR_AMNESIA
           || scroll == SCR_REMOVE_CURSE
           || scroll == SCR_IDENTIFY
           || scroll == SCR_CURSE_ARMOUR
           || scroll == SCR_CURSE_JEWELLERY
           || scroll == SCR_RECHARGING
           || scroll == SCR_ENCHANT_WEAPON
#endif
           || scroll == SCR_BLESS_ITEM
           || scroll == SCR_MAGIC_MAPPING
           || scroll == SCR_ACQUIREMENT;
}

/**
 * Is the player currently able to use the 'r' command (to read books or
 * scrolls). Being too berserk, confused, or having no reading material will
 * prevent this.
 *
 * Prints corresponding messages. (Thanks, canned_msg().)
 */
bool player_can_read()
{
    if (you.berserk())
    {
        canned_msg(MSG_TOO_BERSERK);
        return false;
    }

    if (you.confused())
    {
        canned_msg(MSG_TOO_CONFUSED);
        return false;
    }

    if (you.duration[DUR_BRAINLESS])
    {
        canned_msg(MSG_BRAINLESS);
        return false;
    }

    return true;
}

/**
 * If the player has no items matching the given selector, give an appropriate
 * response to print. Otherwise, if they do have such items, return the empty
 * string.
 */
static string _no_items_reason(object_selector type, bool check_floor = false)
{
    if (!any_items_of_type(type, -1, check_floor))
        return no_selectables_message(type);
    return "";
}

/**
 * If the player is unable to (r)ead the item in the given slot, return the
 * reason why. Otherwise (if they are able to read it), returns "", the empty
 * string.
 */
string cannot_read_item_reason(const item_def &item)
{
    // can read books
    if (item.base_type == OBJ_BOOKS)
        return "";

    // and scrolls - but nothing else.
    if (item.base_type != OBJ_SCROLLS)
        return "You can't read that!";

    // the below only applies to scrolls. (it's easier to read books, since
    // that's just a UI/strategic thing.)

    if (silenced(you.pos()))
        return "Magic scrolls do not work when you're silenced!";

    // water elementals
    if (you.duration[DUR_WATER_HOLD] && !you.res_water_drowning())
        return "You cannot read scrolls while unable to breathe!";

    // ectoplasm
    if (you.duration[DUR_AIR_HOLD] && !you.is_unbreathing())
        return "You cannot read scrolls while unable to breathe!";

    // ru
    if (you.duration[DUR_NO_SCROLLS])
        return "You cannot read scrolls in your current state!";

    // don't waste the player's time reading known scrolls in situations where
    // they'd be useless

    if (!item_type_known(item))
        return "";

    switch (item.sub_type)
    {
        case SCR_BLINKING:
        case SCR_TELEPORTATION:
            return you.no_tele_reason(false, item.sub_type == SCR_BLINKING);

        case SCR_ENCHANT:
            return _no_items_reason(OSEL_ENCHANTABLE_ITEM, true);

#if TAG_MAJOR_VERSION == 34
        case SCR_AMNESIA:
            if (you.spell_no == 0)
                return "You have no spells to forget!";
            return "";

        case SCR_REMOVE_CURSE:
            return _no_items_reason(OSEL_CURSED_WORN);

        case SCR_CURSE_WEAPON:
            if (!you.weapon())
                return "This scroll only affects a wielded weapon!";

            // assumption: wielded weapons always have their curse & brand known
            if (you.weapon()->cursed())
                return "Your weapon is already cursed!";

            if (get_weapon_brand(*you.weapon()) == SPWPN_HOLY_WRATH)
                return "Holy weapons cannot be cursed!";
            return "";

        case SCR_CURSE_ARMOUR:
            return "Removed item can't be used.";

        case SCR_CURSE_JEWELLERY:
            return _no_items_reason(OSEL_UNCURSED_WORN_JEWELLERY);
#endif

        default:
            return "";
    }
}

/**
 * Check if a particular scroll type would hurt a monster.
 *
 * @param scr           Scroll type in question
 * @param m             Actor as a potential victim to the scroll
 * @return  true if the provided scroll type is harmful to the actor.
 */
static bool _scroll_will_harm(const scroll_type scr, const actor &m)
{
    if (!m.alive())
        return false;

    switch (scr)
    {
        case SCR_HOLY_WORD:
            if (m.undead_or_demonic())
                return true;
            break;
        case SCR_TORMENT:
            if (!m.res_torment())
                return true;
            break;
        default: break;
    }

    return false;
}

/**
 * Check to see if the player can read the item in the given slot, and if so,
 * reads it. (Examining books and using scrolls.)
 *
 * @param slot      The slot of the item in the player's inventory. If -1, the
 *                  player is prompted to choose a slot.
 */
void read(item_def* scroll)
{
    if (!player_can_read())
        return;

    if (!scroll)
    {
        scroll = use_an_item(OBJ_SCROLLS, OPER_READ, "Read which item?");
        if (!scroll)
            return;
    }

    const string failure_reason = cannot_read_item_reason(*scroll);
    if (!failure_reason.empty())
    {
        mprf(MSGCH_PROMPT, "%s", failure_reason.c_str());
        return;
    }

    if (scroll->base_type == OBJ_BOOKS)
    {
        _handle_read_book(*scroll);
        return;
    }

    const scroll_type which_scroll = static_cast<scroll_type>(scroll->sub_type);
    // Handle player cancels before we waste time (with e.g. blurryvis)
    if (item_type_known(*scroll)) {
        bool penance = god_hates_item(*scroll);
        string verb_object = "read the " + scroll->name(DESC_DBNAME);

        string penance_prompt = make_stringf("Really %s? This action would"
                                             " place you under penance!",
                                             verb_object.c_str());

        targeter_radius hitfunc(&you, LOS_NO_TRANS);

        if (stop_attack_prompt(hitfunc, verb_object.c_str(),
                               [which_scroll] (const actor* m)
                               {
                                   return _scroll_will_harm(which_scroll, *m);
                               },
                               nullptr, nullptr))
        {
            return;
        }
        else if (penance && !yesno(penance_prompt.c_str(), false, 'n'))
        {
            canned_msg(MSG_OK);
            return;
        }
        else if ((is_dangerous_item(*scroll, true)
                  || is_bad_item(*scroll, true))
                 && Options.bad_item_prompt
                 && !yesno(make_stringf("Really %s?",
                                        verb_object.c_str()).c_str(),
                           false, 'n'))
        {
            canned_msg(MSG_OK);
            return;
        }

        if (scroll->sub_type == SCR_BLINKING
            && orb_limits_translocation()
            && !yesno("Your blink will be uncontrolled - continue anyway?",
                      false, 'n'))
        {
            canned_msg(MSG_OK);
            return;
        }
    }

    if (you.vision() < 0
        && !i_feel_safe(false, false, true)
        && !you.haloed()
        && !yesno("Really read with impaired vision while enemies are nearby?",
                  false, 'n'))
    {
        canned_msg(MSG_OK);
        return;
    }

    if (you.drowning()
        && !i_feel_safe(false, false, true)
        && !yesno("Really read while struggling to swim and enemies are nearby?",
            false, 'n'))
    {
        canned_msg(MSG_OK);
        return;
    }

    // Ok - now we FINALLY get to read a scroll !!! {dlb}
    you.turn_is_over = true;

    // if we have blurry vision, we need to start a delay before the actual
    // scroll effect kicks in.
    if (you.vision() < 0)
    {
        if (you.haloed())
        {
            mpr("The light of the halo makes it easy to see.");
            read_scroll(*scroll);
        }
        else
            start_delay<BlurryScrollDelay>(1, *scroll);
    }
    else if (you.drowning())
    {
        mpr("It's difficult to keep the scroll above water long enough to read it...");
        start_delay<BlurryScrollDelay>(1, *scroll);
    }
    else
        read_scroll(*scroll);
}

/**
 * Read the provided scroll.
 *
 * Does NOT check whether the player can currently read, whether the scroll is
 * currently useless, etc. Likewise doesn't handle blurry vision, setting
 * you.turn_is_over, and other externals. DOES destroy one scroll, unless the
 * player chooses to cancel at the last moment.
 *
 * @param scroll The scroll to be read.
 */
void read_scroll(item_def& scroll)
{
    const scroll_type which_scroll = static_cast<scroll_type>(scroll.sub_type);
    const int prev_quantity = scroll.quantity;
    int link = in_inventory(scroll) ? scroll.link : -1;
    const bool alreadyknown = item_type_known(scroll);

    // For cancellable scrolls leave printing this message to their
    // respective functions.
    const string pre_succ_msg =
            make_stringf("As you read the %s, it crumbles to dust.",
                          scroll.name(DESC_QUALNAME).c_str());
    if (!_is_cancellable_scroll(which_scroll))
    {
        mpr(pre_succ_msg);
        // Actual removal of scroll done afterwards. -- bwr
    }

    const bool dangerous = player_in_a_dangerous_place();

    // ... but some scrolls may still be cancelled afterwards.
    bool cancel_scroll = false;
    bool bad_effect = false; // for Xom: result is bad (or at least dangerous)

    switch (which_scroll)
    {
    case SCR_BLINKING:
    {
        const string reason = you.no_tele_reason(true, true);
        if (!reason.empty())
        {
            mpr(pre_succ_msg);
            mpr(reason);
            break;
        }

        const bool safely_cancellable
            = alreadyknown && (you.vision() >= 0)
             && !you.haloed();

        if (orb_limits_translocation())
        {
            mprf(MSGCH_ORB, "The Orb prevents control of your translocation!");
            uncontrolled_blink();
        }
        else
        {
            cancel_scroll = (cast_controlled_blink(false, safely_cancellable)
                             == spret::abort) && alreadyknown;
        }

        if (!cancel_scroll)
            mpr(pre_succ_msg); // ordering is iffy but w/e
    }
        break;

    case SCR_TELEPORTATION:
        you_teleport();
        break;

    case SCR_ACQUIREMENT:
        if (!alreadyknown)
        {
            mpr(pre_succ_msg);
            mpr("This is a scroll of acquirement!");
        }

        // included in default force_more_message
        // Identify it early in case the player checks the '\' screen.
        set_ident_type(scroll, true);

        if (feat_eliminates_items(grd(you.pos())) || grd(you.pos()) == DNGN_TRAP_SHAFT)
        {
            mpr("Anything you acquired here would fall and be lost!");
            cancel_scroll = true;
            break;
        }

        run_uncancel(UNC_ACQUIREMENT, AQ_SCROLL);
        break;

    case SCR_FEAR:
        mpr("You assume a fearsome visage.");
        mass_enchantment(ENCH_FEAR, 1000);
        break;

    case SCR_ATTENTION:
        you.increase_duration(DUR_SENTINEL_MARK, 40 + random2(40), 80);
        noisy(25, you.pos(), "You hear a loud clanging noise!");
        break;

    case SCR_SUMMONING:
        cast_shadow_creatures(MON_SUMM_SCROLL);
        break;

    case SCR_FOG:
    {
        if (env.level_state & LSTATE_STILL_WINDS)
        {
            mpr("The air is too still for clouds to form.");
            cancel_scroll = true;
            break;
        }
        mpr("The scroll dissolves into smoke.");
        auto smoke = random_smoke_type();
        big_cloud(smoke, &you, you.pos(), 50, 8 + random2(8));
        break;
    }

    case SCR_MAGIC_MAPPING:
        if (alreadyknown && !is_map_persistent())
        {
            cancel_scroll = true;
            mpr("It would have no effect in this place.");
            break;
        }
        mpr(pre_succ_msg);
        magic_mapping(500, 100, false);
        break;

    case SCR_TORMENT:
        torment(&you, TORMENT_SCROLL, you.pos());

        // This is only naughty if you know you're doing it.
        did_god_conduct(DID_EVIL, 10, item_type_known(scroll));
        bad_effect = !player_res_torment(false);
        break;

    case SCR_IMMOLATION:
    {
        bool had_effect = false;
        for (monster_near_iterator mi(you.pos(), LOS_NO_TRANS); mi; ++mi)
        {
            // Don't leak information about Mara and rakshasa clones.
            if (mons_immune_magic(**mi)
                || mi->is_summoned() && !mi->is_illusion())
            {
                continue;
            }

            if (mi->add_ench(mon_enchant(ENCH_INNER_FLAME, 0, &you)))
                had_effect = true;
        }

        if (had_effect)
            mpr("The creatures around you are filled with an inner flame!");
        else
            mpr("The air around you briefly surges with heat, but it dissipates.");

        bad_effect = true;
        break;
    }

    case SCR_BLESS_ITEM:
        cancel_scroll = !_handle_bless_item(pre_succ_msg);
        break;

    case SCR_ENCHANT:
        if (!alreadyknown)
        {
            mpr(pre_succ_msg);
            mpr("It is a scroll of item enchanting.");
            // included in default force_more_message (to show it before menu)
        }
        cancel_scroll =
            (_handle_enchant_item(alreadyknown, pre_succ_msg) == -1);
        break;
#if TAG_MAJOR_VERSION == 34
    // Should always be identified by Ashenzari.
    case SCR_CURSE_ARMOUR:
    case SCR_CURSE_JEWELLERY:
    case SCR_RECHARGING:
    case SCR_IDENTIFY:
    case SCR_ENCHANT_WEAPON:
    case SCR_CURSE_WEAPON:
    case SCR_REMOVE_CURSE:
    {
        mpr("This item has been removed, sorry!");
        cancel_scroll = true;
        break;
    }
#endif

    case SCR_HOLY_WORD:
    {
        holy_word(100, HOLY_WORD_SCROLL, you.pos(), false, &you);

        // This is always naughty, even if you didn't affect anyone.
        // Don't speak those foul holy words even in jest!
        did_god_conduct(DID_HOLY, 10, item_type_known(scroll));
        bad_effect = you.undead_or_demonic();
        break;
    }

    case SCR_SILENCE:
        cast_silence(30);
        break;

    case SCR_VULNERABILITY:
        _vulnerability_scroll();
        break;

    case SCR_AMNESIA:
        if (!alreadyknown)
        {
            mpr(pre_succ_msg);
            mpr("It is a scroll of amnesia.");
            // included in default force_more_message (to show it before menu)
        }
        if (you.spell_no == 0)
            mpr("You feel forgetful for a moment.");
        else
        {
            bool done;
            bool aborted;
            do
            {
                aborted = cast_selective_amnesia() == -1;
                done = !aborted
                       || alreadyknown
                       || crawl_state.seen_hups
                       || yesno("Really abort (and waste the scroll)?",
                                false, 0);
                cancel_scroll = aborted && alreadyknown;
            } while (!done);
            if (aborted)
                canned_msg(MSG_OK);
        }
        break;

    case SCR_RANDOM_USELESSNESS:
        random_uselessness(&you);
        break;

    default:
        mpr("Read a buggy scroll, please report this.");
        break;
    }

    if (cancel_scroll)
        you.turn_is_over = false;

    set_ident_type(scroll, true);
    set_ident_flags(scroll, ISFLAG_KNOW_TYPE); // for notes

    string scroll_name = scroll.name(DESC_QUALNAME);

    if (!cancel_scroll)
    {
        if (in_inventory(scroll))
            dec_inv_item_quantity(link, 1);
        else
            dec_mitm_item_quantity(scroll.index(), 1);
        count_action(CACT_USE, OBJ_SCROLLS);
    }

    if (!alreadyknown
        && which_scroll != SCR_ACQUIREMENT
        && which_scroll != SCR_BLESS_ITEM
        && which_scroll != SCR_ENCHANT
#if TAG_MAJOR_VERSION == 34
        && which_scroll != SCR_ENCHANT_WEAPON
        && which_scroll != SCR_RECHARGING
        && which_scroll != SCR_IDENTIFY
        && which_scroll != SCR_AMNESIA)
#endif
    {
        mprf("It %s a %s.",
             scroll.quantity < prev_quantity ? "was" : "is",
             scroll_name.c_str());
    }

    if (!alreadyknown && dangerous)
    {
        // Xom loves it when you read an unknown scroll and there is a
        // dangerous monster nearby... (though not as much as potions
        // since there are no *really* bad scrolls, merely useless ones).
        xom_is_stimulated(bad_effect ? 100 : 50);
    }

    if (!alreadyknown)
        auto_assign_item_slot(scroll);

}

#ifdef USE_TILE
// Interactive menu for item drop/use.

void tile_item_use_floor(int idx)
{
    if (mitm[idx].is_type(OBJ_CORPSES, CORPSE_BODY))
        butchery(&mitm[idx]);
}

void tile_item_pickup(int idx, bool part)
{
    if (item_is_stationary(mitm[idx]))
    {
        mpr("You can't pick that up.");
        return;
    }

    if (part)
    {
        pickup_menu(idx);
        return;
    }
    pickup_single_item(idx, -1);
}

void tile_item_drop(int idx, bool partdrop)
{
    int quantity = you.inv[idx].quantity;
    if (partdrop && quantity > 1)
    {
        quantity = prompt_for_int("Drop how many? ", true);
        if (quantity < 1)
        {
            canned_msg(MSG_OK);
            return;
        }
        if (quantity > you.inv[idx].quantity)
            quantity = you.inv[idx].quantity;
    }
    drop_item(idx, quantity);
}

void tile_item_eat_floor(int idx)
{
    if (can_eat(mitm[idx], false))
        eat_item(mitm[idx]);
}

void tile_item_use_secondary(int idx)
{
    const item_def item = you.inv[idx];

    if (you.equip[EQ_WEAPON0] == idx)
        wield_weapon(true, SLOT_BARE_HANDS);
    else if (item_is_wieldable(item))
    {
        // secondary wield for several spells and such
        wield_weapon(true, idx); // wield
    }
}

void tile_item_use(int idx)
{
    const item_def item = you.inv[idx];

    // Equipped?
    bool equipped = false;
    for (unsigned int i = EQ_FIRST_EQUIP; i < NUM_EQUIP; i++)
    {
        if (you.equip[i] == idx)
        {
            equipped = true;
            break;
        }
    }

    const int type = item.base_type;

    // Use it
    switch (type)
    {
        case OBJ_WEAPONS:
        case OBJ_STAVES:
        case OBJ_MISCELLANY:
        case OBJ_SHIELDS:
        case OBJ_WANDS:
            // Wield any unwielded item of these types.
            if (!equipped && item_is_wieldable(item))
            {
                wield_weapon(true, idx);
                return;
            }
            // Evoke misc. items or wands.
            if (item_is_evokable(item, false))
            {
                evoke_item(idx);
                return;
            }
            // Unwield wielded items.
            if (equipped)
                wield_weapon(true, SLOT_BARE_HANDS);
            return;

        case OBJ_MISSILES:
            return;

        case OBJ_ARMOURS:
            if (!form_can_wear())
            {
                mpr("You can't wear or remove anything in your present form.");
                return;
            }
            if (equipped)
            {
                if (check_warning_inscriptions(item, OPER_TAKEOFF))
                    takeoff_armour(idx);
            }
            else if (check_warning_inscriptions(item, OPER_WEAR))
                wear_armour(idx);
            return;

        case OBJ_CORPSES:
            if (you.species != SP_VAMPIRE
                || item.sub_type == CORPSE_SKELETON)
            {
                break;
            }
            // intentional fall-through for Vampires
        case OBJ_FOOD:
            if (check_warning_inscriptions(item, OPER_EAT))
                eat_food(idx);
            return;

        case OBJ_SCROLLS:
            if (check_warning_inscriptions(item, OPER_READ))
                read(&you.inv[idx]);
            return;

        case OBJ_JEWELLERY:
            if (equipped)
                remove_ring(idx);
            else if (check_warning_inscriptions(item, OPER_PUTON))
                puton_ring(idx);
            return;

        case OBJ_POTIONS:
            if (check_warning_inscriptions(item, OPER_QUAFF))
                drink(&you.inv[idx]);
            return;

        default:
            return;
    }
}
#endif
