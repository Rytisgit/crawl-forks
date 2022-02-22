#include "AppHdr.h"

#ifdef USE_TILE
#include "tilepick-p.h"

#include <cstdio>

#include "artefact.h"
#include "describe.h"
#include "item-name.h"
#include "item-prop.h"
#include "player.h"
#include "tile-flags.h"
#include "tile-player-flag-cut.h"
#include "rltiles/tiledef-player.h"
#include "rltiles/tiledef-unrand.h"
#include "tiledoll.h"
#include "tilepick.h"
#include "transform.h"
#include "traps.h"

static tileidx_t _modrng(int mod, tileidx_t first, tileidx_t last)
{
    return first + mod % (last - first + 1);
}

static tileidx_t _mon_mod(tileidx_t tile, int offset)
{
    int count = tile_player_count(tile);
    return tile + offset % count;
}

tileidx_t tilep_equ_hand1(const item_def &item)
{
    if (item.props.exists("worn_tile"))
        return item.props["worn_tile"].get_short();

#if TAG_MAJOR_VERSION == 34
    if (item.base_type == OBJ_RODS)
        return _mon_mod(TILEP_HAND1_ROD_FIRST, item.rnd);
#endif

    if (item.base_type == OBJ_MISCELLANY)
    {
        switch (item.sub_type)
        {
#if TAG_MAJOR_VERSION == 34
        case MISC_BOTTLED_EFREET:             return TILEP_HAND1_BOTTLE;
#endif
        case MISC_FAN_OF_GALES:               return TILEP_HAND1_FAN;
#if TAG_MAJOR_VERSION == 34
        case MISC_STONE_OF_TREMORS:           return TILEP_HAND1_STONE;
#endif
        case MISC_LIGHTNING_ROD:              return 0;

        case MISC_CRYSTAL_BALL_OF_ENERGY:     return TILEP_HAND1_CRYSTAL;

        case MISC_LAMP_OF_FIRE:               return TILEP_HAND1_LANTERN;
        case MISC_LANTERN_OF_SHADOWS:         return TILEP_HAND1_BONE_LANTERN;
        case MISC_HORN_OF_GERYON:             return TILEP_HAND1_HORN;
        case MISC_BOX_OF_BEASTS:              return TILEP_HAND1_BOX;

        case MISC_DECK_OF_ESCAPE:
        case MISC_DECK_OF_DESTRUCTION:
#if TAG_MAJOR_VERSION == 34
        case MISC_DECK_OF_DUNGEONS:
#endif
        case MISC_DECK_OF_SUMMONING:
#if TAG_MAJOR_VERSION == 34
        case MISC_DECK_OF_WONDERS:
#endif
        case MISC_DECK_OF_PUNISHMENT:
#if TAG_MAJOR_VERSION == 34
        case MISC_DECK_OF_WAR:
        case MISC_DECK_OF_CHANGES:
        case MISC_DECK_OF_DEFENCE:
#endif
            return TILEP_HAND1_DECK;
        }
    }

    if (is_unrandom_artefact(item))
    {
        const tileidx_t tile = unrandart_to_right_hand(find_unrandart_index(item));
        if (tile)
            return tile;
    }

    if (item.base_type == OBJ_STAVES)
    {
        int orig_special = you.item_description[IDESC_STAVES][item.sub_type];
        int desc = (orig_special / NDSC_STAVE_PRI) % NDSC_STAVE_SEC;
        return TILEP_HAND1_STAFF_LARGE + desc;
    }

    if (item.base_type == OBJ_SHIELDS)
    {
        switch (item.sub_type)
        {
        case SHD_SHIELD:
            return _modrng(item.rnd, TILEP_HAND1_SHIELD_KNIGHT_BLUE,
                TILEP_HAND1_SHIELD_KITE4);
        case SHD_BUCKLER:
            return _modrng(item.rnd, TILEP_HAND1_BUCKLER_ROUND,
                TILEP_HAND1_BUCKLER_SPIRAL);
        case SHD_LARGE_SHIELD:
            return _modrng(item.rnd, TILEP_HAND1_LARGE_SHIELD_LONG_RED,
                TILEP_HAND1_LARGE_SHIELD_SPIRAL);
        case SHD_NUNCHAKU:
            return (TILEP_HAND1_FLAIL_STICK_SLANT);
        case SHD_TARGE:
            return _modrng(item.rnd, TILEP_HAND1_TARGE0,
                TILEP_HAND1_TARGE5);
        case SHD_SAI:
            return (TILEP_HAND1_SAI);
        default: return 0;
        }
    }

    if (item.base_type != OBJ_WEAPONS)
        return 0;

    tileidx_t tile = 0;

    switch (item.sub_type)
    {
        // Blunt
    case WPN_CLUB:
        tile = TILEP_HAND1_CLUB_SLANT;
        break;
    case WPN_MACE:
        tile = TILEP_HAND1_MACE;
        break;
    case WPN_GREAT_MACE:
        tile = TILEP_HAND1_GREAT_MACE;
        break;
    case WPN_FLAIL:
        tile = TILEP_HAND1_FLAIL;
        break;
    case WPN_DIRE_FLAIL:
        tile = TILEP_HAND1_GREAT_FLAIL;
        break;
    case WPN_MORNINGSTAR:
        tile = TILEP_HAND1_MORNINGSTAR;
        break;
    case WPN_EVENINGSTAR:
        tile = TILEP_HAND1_EVENINGSTAR;
        break;
    case WPN_GIANT_CLUB:
        tile = TILEP_HAND1_GIANT_CLUB_PLAIN;
        break;
    case WPN_GIANT_SPIKED_CLUB:
        tile = TILEP_HAND1_GIANT_CLUB_SPIKE_SLANT;
        break;
    case WPN_WHIP:
        tile = TILEP_HAND1_WHIP;
        break;
    case WPN_DEMON_WHIP:
        tile = TILEP_HAND1_BLACK_WHIP;
        break;
    case WPN_SACRED_SCOURGE:
        tile = TILEP_HAND1_SACRED_SCOURGE;
        break;

        // Edge
    case WPN_DAGGER:
        tile = TILEP_HAND1_DAGGER_SLANT;
        break;
    case WPN_CLEAVER:
        tile = TILEP_HAND1_CLEAVER;
        break;
    case WPN_SHORT_SWORD:
        tile = TILEP_HAND1_SHORT_SWORD_SLANT;
        break;
    case WPN_LONG_SWORD:
        tile = TILEP_HAND1_LONG_SWORD_SLANT;
        break;
    case WPN_GREAT_SWORD:
        tile = TILEP_HAND1_GREAT_SWORD_SLANT;
        break;
    case WPN_SCIMITAR:
        tile = TILEP_HAND1_SCIMITAR;
        break;
    case WPN_FALCHION:
        tile = TILEP_HAND1_FALCHION;
        break;
    case WPN_RAPIER:
        tile = TILEP_HAND1_RAPIER;
        break;
    case WPN_DEMON_BLADE:
        tile = TILEP_HAND1_DEMON_BLADE;
        break;
    case WPN_TANTO:
        tile = TILEP_HAND1_DAGGER;
        break;
    case WPN_KRIS:
        tile = TILEP_HAND1_KRIS;
        break;
    case WPN_KATAR:
        tile = TILEP_HAND1_KATAR;
        break;
    case WPN_DOUBLE_SWORD:
        tile = TILEP_HAND1_DOUBLE_SWORD;
        break;
    case WPN_TRIPLE_SWORD:
        tile = TILEP_HAND1_TRIPLE_SWORD;
        break;
    case WPN_EUDEMON_BLADE:
        tile = TILEP_HAND1_BLESSED_BLADE;
        break;

        // Axes and Hammers
    case WPN_HAND_AXE:
        tile = TILEP_HAND1_HAND_AXE;
        break;
    case WPN_HAMMER:
        tile = TILEP_HAND1_HAMMER;
        break;
    case WPN_BATTLEAXE:
        tile = TILEP_HAND1_BATTLEAXE;
        break;
    case WPN_BROAD_AXE:
        tile = TILEP_HAND1_BROAD_AXE;
        break;
    case WPN_WAR_AXE:
        tile = TILEP_HAND1_WAR_AXE;
        break;
    case WPN_PICKAXE:
        tile = TILEP_HAND1_PICK_AXE;
        break;
    case WPN_EXECUTIONERS_AXE:
        tile = TILEP_HAND1_EXECUTIONERS_AXE;
        break;
    case WPN_BARDICHE:
        tile = TILEP_HAND1_GLAIVE3;
        break;

        // Pole
    case WPN_SPEAR:
        tile = TILEP_HAND1_SPEAR;
        break;
    case WPN_HALBERD:
        tile = TILEP_HAND1_HALBERD;
        break;
    case WPN_GLAIVE:
        tile = TILEP_HAND1_GLAIVE;
        break;
#if TAG_MAJOR_VERSION == 34
    case WPN_STAFF:
        tile = TILEP_HAND1_STAFF;
        break;
#endif
    case WPN_QUARTERSTAFF:
        tile = TILEP_HAND1_QUARTERSTAFF1;
        break;
    case WPN_LAJATANG:
        tile = TILEP_HAND1_LAJATANG;
        break;

    case WPN_SCYTHE:
        tile = TILEP_HAND1_SCYTHE;
        break;
    case WPN_TRIDENT:
        tile = TILEP_HAND1_TRIDENT2;
        break;
    case WPN_DEMON_TRIDENT:
        tile = TILEP_HAND1_DEMON_TRIDENT;
        break;
    case WPN_TRISHULA:
        tile = TILEP_HAND1_TRISHULA;
        break;

        // Ranged
    case WPN_HUNTING_SLING:
        tile = TILEP_HAND1_HUNTING_SLING;
        break;
    case WPN_FUSTIBALUS:
        tile = TILEP_HAND1_FUSTIBALUS;
        break;
    case WPN_MANGONEL:
        tile = TILEP_HAND1_MANGONEL;
        break;
    case WPN_SHORTBOW:
        tile = TILEP_HAND1_BOW2;
        break;
    case WPN_HAND_CROSSBOW:
        tile = TILEP_HAND1_HAND_CROSSBOW;
        break;
    case WPN_ARBALEST:
        tile = TILEP_HAND1_ARBALEST;
        break;
    case WPN_TRIPLE_CROSSBOW:
        tile = TILEP_HAND1_TRIPLE_CROSSBOW;
        break;
    case WPN_LONGBOW:
        tile = TILEP_HAND1_BOW3;
        break;

    default: tile = 0;
    }

    return tile ? tileidx_enchant_equ(item, tile, true) : 0;
}

tileidx_t tilep_equ_hand2(const item_def &item)
{
    if (item.props.exists("worn_tile"))
        return item.props["worn_tile"].get_short();

#if TAG_MAJOR_VERSION == 34
    if (item.base_type == OBJ_RODS)
        return _mon_mod(TILEP_HAND2_ROD_FIRST, item.rnd);
#endif

    if (item.base_type == OBJ_MISCELLANY)
    {
        switch (item.sub_type)
        {
#if TAG_MAJOR_VERSION == 34
        case MISC_BOTTLED_EFREET:             return TILEP_HAND2_BOTTLE;
#endif
        case MISC_FAN_OF_GALES:               return TILEP_HAND2_FAN;
#if TAG_MAJOR_VERSION == 34
        case MISC_STONE_OF_TREMORS:           return TILEP_HAND2_STONE;
#endif
        case MISC_LIGHTNING_ROD:              return 0;

        case MISC_CRYSTAL_BALL_OF_ENERGY:     return TILEP_HAND2_CRYSTAL;

        case MISC_LAMP_OF_FIRE:               return TILEP_HAND2_LANTERN;
        case MISC_LANTERN_OF_SHADOWS:         return TILEP_HAND2_BONE_LANTERN;
        case MISC_HORN_OF_GERYON:             return TILEP_HAND2_HORN;
        case MISC_BOX_OF_BEASTS:              return TILEP_HAND2_BOX;

        case MISC_DECK_OF_ESCAPE:
        case MISC_DECK_OF_DESTRUCTION:
#if TAG_MAJOR_VERSION == 34
        case MISC_DECK_OF_DUNGEONS:
#endif
        case MISC_DECK_OF_SUMMONING:
#if TAG_MAJOR_VERSION == 34
        case MISC_DECK_OF_WONDERS:
#endif
        case MISC_DECK_OF_PUNISHMENT:
#if TAG_MAJOR_VERSION == 34
        case MISC_DECK_OF_WAR:
        case MISC_DECK_OF_CHANGES:
        case MISC_DECK_OF_DEFENCE:
#endif
            return TILEP_HAND2_DECK;
        }
    }

    if (is_unrandom_artefact(item))
    {
        const tileidx_t tile = unrandart_to_left_hand(find_unrandart_index(item));
        if (tile)
            return tile;
    }

    if (item.base_type == OBJ_STAVES)
    {
        int orig_special = you.item_description[IDESC_STAVES][item.sub_type];
        int desc = (orig_special / NDSC_STAVE_PRI) % NDSC_STAVE_SEC;
        return TILEP_HAND2_STAFF_LARGE + desc;
    }

    if (item.base_type == OBJ_SHIELDS)
    {
        switch (item.sub_type)
        {
        case SHD_SHIELD:
            return _modrng(item.rnd, TILEP_HAND2_SHIELD_KNIGHT_BLUE,
                TILEP_HAND2_SHIELD_KITE4);
        case SHD_BUCKLER:
            return _modrng(item.rnd, TILEP_HAND2_BUCKLER_ROUND,
                TILEP_HAND2_BUCKLER_SPIRAL);
        case SHD_LARGE_SHIELD:
            return _modrng(item.rnd, TILEP_HAND2_LARGE_SHIELD_LONG_RED,
                TILEP_HAND2_LARGE_SHIELD_SPIRAL);
        case SHD_NUNCHAKU:
            return (TILEP_HAND2_FLAIL_STICK_SLANT);
        case SHD_TARGE:
            return _modrng(item.rnd, TILEP_HAND2_TARGE0,
                TILEP_HAND2_TARGE5);
        case SHD_SAI:
            return (TILEP_HAND2_SAI);
        default: return 0;
        }
    }

    if (item.base_type != OBJ_WEAPONS)
        return 0;

    tileidx_t tile = 0;

    switch (item.sub_type)
    {
        // Blunt
    case WPN_CLUB:
        tile = TILEP_HAND2_CLUB_SLANT;
        break;
    case WPN_MACE:
        tile = TILEP_HAND2_MACE;
        break;
    case WPN_GREAT_MACE:
        tile = TILEP_HAND2_GREAT_MACE;
        break;
    case WPN_FLAIL:
        tile = TILEP_HAND2_FLAIL;
        break;
    case WPN_DIRE_FLAIL:
        tile = TILEP_HAND2_GREAT_FLAIL;
        break;
    case WPN_MORNINGSTAR:
        tile = TILEP_HAND2_MORNINGSTAR;
        break;
    case WPN_EVENINGSTAR:
        tile = TILEP_HAND2_EVENINGSTAR;
        break;
    case WPN_GIANT_CLUB:
        tile = TILEP_HAND2_GIANT_CLUB_PLAIN;
        break;
    case WPN_GIANT_SPIKED_CLUB:
        tile = TILEP_HAND2_GIANT_CLUB_SPIKE_SLANT;
        break;
    case WPN_WHIP:
        tile = TILEP_HAND2_WHIP;
        break;
    case WPN_DEMON_WHIP:
        tile = TILEP_HAND2_BLACK_WHIP;
        break;
    case WPN_SACRED_SCOURGE:
        tile = TILEP_HAND2_SACRED_SCOURGE;
        break;

        // Edge
    case WPN_DAGGER:
        tile = TILEP_HAND2_DAGGER_SLANT;
        break;
    case WPN_CLEAVER:
        tile = TILEP_HAND2_CLEAVER;
        break;
    case WPN_SHORT_SWORD:
        tile = TILEP_HAND2_SHORT_SWORD_SLANT;
        break;
    case WPN_LONG_SWORD:
        tile = TILEP_HAND2_LONG_SWORD_SLANT;
        break;
    case WPN_GREAT_SWORD:
        tile = TILEP_HAND2_GREAT_SWORD_SLANT;
        break;
    case WPN_SCIMITAR:
        tile = TILEP_HAND2_SCIMITAR;
        break;
    case WPN_FALCHION:
        tile = TILEP_HAND2_FALCHION;
        break;
    case WPN_RAPIER:
        tile = TILEP_HAND2_RAPIER;
        break;
    case WPN_DEMON_BLADE:
        tile = TILEP_HAND2_DEMON_BLADE;
        break;
    case WPN_TANTO:
        tile = TILEP_HAND2_DAGGER;
        break;
    case WPN_KRIS:
        tile = TILEP_HAND2_KRIS;
        break;
    case WPN_KATAR:
        tile = TILEP_HAND2_KATAR;
        break;
    case WPN_DOUBLE_SWORD:
        tile = TILEP_HAND2_DOUBLE_SWORD;
        break;
    case WPN_TRIPLE_SWORD:
        tile = TILEP_HAND2_TRIPLE_SWORD;
        break;
    case WPN_EUDEMON_BLADE:
        tile = TILEP_HAND2_BLESSED_BLADE;
        break;

        // Axes and hammers
    case WPN_HAND_AXE:
        tile = TILEP_HAND2_HAND_AXE;
        break;
    case WPN_HAMMER:
        tile = TILEP_HAND2_HAMMER;
        break;
    case WPN_BATTLEAXE:
        tile = TILEP_HAND2_BATTLEAXE;
        break;
    case WPN_BROAD_AXE:
        tile = TILEP_HAND2_BROAD_AXE;
        break;
    case WPN_WAR_AXE:
        tile = TILEP_HAND2_WAR_AXE;
        break;
    case WPN_PICKAXE:
        tile = TILEP_HAND2_PICK_AXE;
        break;
    case WPN_EXECUTIONERS_AXE:
        tile = TILEP_HAND2_EXECUTIONERS_AXE;
        break;
    case WPN_BARDICHE:
        tile = TILEP_HAND2_GLAIVE3;
        break;

        // Pole
    case WPN_SPEAR:
        tile = TILEP_HAND2_SPEAR;
        break;
    case WPN_HALBERD:
        tile = TILEP_HAND2_HALBERD;
        break;
    case WPN_GLAIVE:
        tile = TILEP_HAND2_GLAIVE;
        break;
#if TAG_MAJOR_VERSION == 34
    case WPN_STAFF:
        tile = TILEP_HAND2_STAFF;
        break;
#endif
    case WPN_QUARTERSTAFF:
        tile = TILEP_HAND2_QUARTERSTAFF1;
        break;
    case WPN_LAJATANG:
        tile = TILEP_HAND2_LAJATANG;
        break;

    case WPN_SCYTHE:
        tile = TILEP_HAND2_SCYTHE;
        break;
    case WPN_TRIDENT:
        tile = TILEP_HAND2_TRIDENT2;
        break;
    case WPN_DEMON_TRIDENT:
        tile = TILEP_HAND2_DEMON_TRIDENT;
        break;
    case WPN_TRISHULA:
        tile = TILEP_HAND2_TRISHULA;
        break;

        // Ranged
    case WPN_HUNTING_SLING:
        tile = TILEP_HAND2_HUNTING_SLING;
        break;
    case WPN_FUSTIBALUS:
        tile = TILEP_HAND2_FUSTIBALUS;
        break;
    case WPN_MANGONEL:
        tile = TILEP_HAND2_MANGONEL;
        break;
    case WPN_SHORTBOW:
        tile = TILEP_HAND2_BOW2;
        break;
    case WPN_HAND_CROSSBOW:
        tile = TILEP_HAND2_HAND_CROSSBOW;
        break;
    case WPN_ARBALEST:
        tile = TILEP_HAND2_ARBALEST;
        break;
    case WPN_TRIPLE_CROSSBOW:
        tile = TILEP_HAND2_TRIPLE_CROSSBOW;
        break;
    case WPN_LONGBOW:
        tile = TILEP_HAND2_BOW3;
        break;

    default: tile = 0;
    }

    return tile ? tileidx_enchant_equ(item, tile, true) : 0;
}

tileidx_t tilep_equ_weapon(const item_def &item)
{
    if (item.props.exists("worn_tile"))
        return item.props["worn_tile"].get_short();

    if (item.base_type == OBJ_STAVES)
    {
        int orig_special = you.item_description[IDESC_STAVES][item.sub_type];
        int desc = (orig_special / NDSC_STAVE_PRI) % NDSC_STAVE_SEC;
        return TILEP_HAND1_STAFF_LARGE + desc;
    }

#if TAG_MAJOR_VERSION == 34
    if (item.base_type == OBJ_RODS)
        return _mon_mod(TILEP_HAND1_ROD_FIRST, item.rnd);
#endif

    if (item.base_type == OBJ_MISCELLANY)
    {
        switch (item.sub_type)
        {
#if TAG_MAJOR_VERSION == 34
        case MISC_BOTTLED_EFREET:             return TILEP_HAND1_BOTTLE;
#endif
        case MISC_FAN_OF_GALES:               return TILEP_HAND1_FAN;
#if TAG_MAJOR_VERSION == 34
        case MISC_STONE_OF_TREMORS:           return TILEP_HAND1_STONE;
#endif
        case MISC_LIGHTNING_ROD:              return 0;

        case MISC_CRYSTAL_BALL_OF_ENERGY:     return TILEP_HAND1_CRYSTAL;

        case MISC_LAMP_OF_FIRE:               return TILEP_HAND1_LANTERN;
        case MISC_LANTERN_OF_SHADOWS:         return TILEP_HAND1_BONE_LANTERN;
        case MISC_HORN_OF_GERYON:             return TILEP_HAND1_HORN;
        case MISC_BOX_OF_BEASTS:              return TILEP_HAND1_BOX;

        case MISC_DECK_OF_ESCAPE:
        case MISC_DECK_OF_DESTRUCTION:
#if TAG_MAJOR_VERSION == 34
        case MISC_DECK_OF_DUNGEONS:
#endif
        case MISC_DECK_OF_SUMMONING:
#if TAG_MAJOR_VERSION == 34
        case MISC_DECK_OF_WONDERS:
#endif
        case MISC_DECK_OF_PUNISHMENT:
#if TAG_MAJOR_VERSION == 34
        case MISC_DECK_OF_WAR:
        case MISC_DECK_OF_CHANGES:
        case MISC_DECK_OF_DEFENCE:
#endif
            return TILEP_HAND1_DECK;
        }
    }

    if (item.base_type == OBJ_SHIELDS)
    {
        switch (item.sub_type)
        {
        case SHD_SHIELD:
            return _modrng(item.rnd, TILEP_HAND2_SHIELD_KNIGHT_BLUE,
                TILEP_HAND2_SHIELD_KITE4);
        case SHD_BUCKLER:
            return _modrng(item.rnd, TILEP_HAND2_BUCKLER_ROUND,
                TILEP_HAND2_BUCKLER_SPIRAL);
        case SHD_LARGE_SHIELD:
            return _modrng(item.rnd, TILEP_HAND2_LARGE_SHIELD_LONG_RED,
                TILEP_HAND2_LARGE_SHIELD_SPIRAL);
        case SHD_NUNCHAKU:
            return (TILEP_HAND2_FLAIL_STICK_SLANT);
        case SHD_TARGE:
            return _modrng(item.rnd, TILEP_HAND2_TARGE0,
                TILEP_HAND2_TARGE5);
        case SHD_SAI:
            return (TILEP_HAND2_SAI);
        default: return 0;
        }
    }

    if (is_unrandom_artefact(item))
    {
        if (item.base_type == OBJ_SHIELDS)
        {
            const tileidx_t tile0 = unrandart_to_left_hand(find_unrandart_index(item));
            if (tile0)
                return tile0;
        }
        if (item.base_type == OBJ_WEAPONS)
        {
            const tileidx_t tile1 = unrandart_to_right_hand(find_unrandart_index(item));
            if (tile1)
                return tile1;
        }
    }

    if (item.base_type != OBJ_WEAPONS)
        return 0;

    tileidx_t tile = 0;

    switch (item.sub_type)
    {
    // Blunt
    case WPN_CLUB:
        tile = TILEP_HAND1_CLUB_SLANT;
        break;
    case WPN_MACE:
        tile = TILEP_HAND1_MACE;
        break;
    case WPN_GREAT_MACE:
        tile = TILEP_HAND1_GREAT_MACE;
        break;
    case WPN_FLAIL:
        tile = TILEP_HAND1_FLAIL;
        break;
    case WPN_DIRE_FLAIL:
        tile = TILEP_HAND1_GREAT_FLAIL;
        break;
    case WPN_MORNINGSTAR:
        tile = TILEP_HAND1_MORNINGSTAR;
        break;
    case WPN_EVENINGSTAR:
        tile = TILEP_HAND1_EVENINGSTAR;
        break;
    case WPN_GIANT_CLUB:
        tile = TILEP_HAND1_GIANT_CLUB_PLAIN;
        break;
    case WPN_GIANT_SPIKED_CLUB:
        tile = TILEP_HAND1_GIANT_CLUB_SPIKE_SLANT;
        break;
    case WPN_WHIP:
        tile = TILEP_HAND1_WHIP;
        break;
    case WPN_DEMON_WHIP:
        tile = TILEP_HAND1_BLACK_WHIP;
        break;
    case WPN_SACRED_SCOURGE:
        tile = TILEP_HAND1_SACRED_SCOURGE;
        break;

    // Edge
    case WPN_DAGGER:
        tile = TILEP_HAND1_DAGGER_SLANT;
        break;
    case WPN_CLEAVER:
        tile = TILEP_HAND1_CLEAVER;
        break;
    case WPN_SHORT_SWORD:
        tile = TILEP_HAND1_SHORT_SWORD_SLANT;
        break;
    case WPN_LONG_SWORD:
        tile = TILEP_HAND1_LONG_SWORD_SLANT;
        break;
    case WPN_GREAT_SWORD:
        tile = TILEP_HAND1_GREAT_SWORD_SLANT;
        break;
    case WPN_SCIMITAR:
        tile = TILEP_HAND1_SCIMITAR;
        break;
    case WPN_FALCHION:
        tile = TILEP_HAND1_FALCHION;
        break;
    case WPN_RAPIER:
        tile = TILEP_HAND1_RAPIER;
        break;
    case WPN_DEMON_BLADE:
        tile = TILEP_HAND1_DEMON_BLADE;
        break;
    case WPN_TANTO:
        tile = TILEP_HAND1_DAGGER;
        break;
    case WPN_KRIS:
        tile = TILEP_HAND1_KRIS;
        break;
    case WPN_KATAR:
        tile = TILEP_HAND1_KATAR;
        break;
    case WPN_DOUBLE_SWORD:
        tile = TILEP_HAND1_DOUBLE_SWORD;
        break;
    case WPN_TRIPLE_SWORD:
        tile = TILEP_HAND1_TRIPLE_SWORD;
        break;
    case WPN_EUDEMON_BLADE:
        tile = TILEP_HAND1_BLESSED_BLADE;
        break;

    // Axes and hammers
    case WPN_HAND_AXE:
        tile = TILEP_HAND1_HAND_AXE;
        break;
    case WPN_HAMMER:
        tile = TILEP_HAND1_HAMMER;
        break;
    case WPN_BATTLEAXE:
        tile = TILEP_HAND1_BATTLEAXE;
        break;
    case WPN_BROAD_AXE:
        tile = TILEP_HAND1_BROAD_AXE;
        break;
    case WPN_WAR_AXE:
        tile = TILEP_HAND1_WAR_AXE;
        break;
    case WPN_PICKAXE:
        tile = TILEP_HAND1_PICK_AXE;
        break;
    case WPN_EXECUTIONERS_AXE:
        tile = TILEP_HAND1_EXECUTIONERS_AXE;
        break;
    case WPN_BARDICHE:
        tile = TILEP_HAND1_GLAIVE3;
        break;

    // Pole
    case WPN_SPEAR:
        tile = TILEP_HAND1_SPEAR;
        break;
    case WPN_HALBERD:
        tile = TILEP_HAND1_HALBERD;
        break;
    case WPN_GLAIVE:
        tile = TILEP_HAND1_GLAIVE;
        break;
#if TAG_MAJOR_VERSION == 34
    case WPN_STAFF:
        tile = TILEP_HAND1_STAFF;
        break;
#endif
    case WPN_QUARTERSTAFF:
        tile = TILEP_HAND1_QUARTERSTAFF1;
        break;
    case WPN_LAJATANG:
        tile = TILEP_HAND1_LAJATANG;
        break;

    case WPN_SCYTHE:
        tile = TILEP_HAND1_SCYTHE;
        break;
    case WPN_TRIDENT:
        tile = TILEP_HAND1_TRIDENT2;
        break;
    case WPN_DEMON_TRIDENT:
        tile = TILEP_HAND1_DEMON_TRIDENT;
        break;
    case WPN_TRISHULA:
        tile = TILEP_HAND1_TRISHULA;
        break;

    // Ranged
    case WPN_HUNTING_SLING:
        tile = TILEP_HAND1_HUNTING_SLING;
        break;
    case WPN_FUSTIBALUS:
        tile = TILEP_HAND1_FUSTIBALUS;
        break;
    case WPN_MANGONEL:
        tile = TILEP_HAND1_MANGONEL;
        break;
    case WPN_SHORTBOW:
        tile = TILEP_HAND1_BOW2;
        break;
    case WPN_HAND_CROSSBOW:
        tile = TILEP_HAND1_HAND_CROSSBOW;
        break;
    case WPN_ARBALEST:
        tile = TILEP_HAND1_ARBALEST;
        break;
    case WPN_TRIPLE_CROSSBOW:
        tile = TILEP_HAND1_TRIPLE_CROSSBOW;
        break;
    case WPN_LONGBOW:
        tile = TILEP_HAND1_BOW3;
        break;

    default: tile = 0;
    }

    return tile ? tileidx_enchant_equ(item, tile, true) : 0;
}

tileidx_t tilep_equ_armour(const item_def &item)
{
    if (item.base_type != OBJ_ARMOURS)
        return 0;

    if (item.props.exists("worn_tile"))
        return item.props["worn_tile"].get_short();

    if (is_unrandom_artefact(item))
    {
        const tileidx_t tile = unrandart_to_doll_tile(find_unrandart_index(item));
        if (tile)
            return tile;
    }

    if (item.sub_type == ARM_ROBE)
    {
        return _modrng(item.rnd, TILEP_BODY_ROBE_FIRST_NORM,
                       TILEP_BODY_ROBE_LAST_NORM);
    }

    tileidx_t tile = 0;
    switch (item.sub_type)
    {
    case ARM_LEATHER_ARMOUR:        tile = TILEP_BODY_LEATHER_ARMOUR; break;
    case ARM_RING_MAIL:             tile = TILEP_BODY_RINGMAIL; break;
    case ARM_CHAIN_MAIL:            tile = TILEP_BODY_CHAINMAIL; break;
    case ARM_SCALE_MAIL:            tile = TILEP_BODY_SCALEMAIL; break;
    case ARM_PLATE_ARMOUR:          tile = TILEP_BODY_PLATE; break;
    case ARM_CRYSTAL_PLATE_ARMOUR:  tile = TILEP_BODY_CRYSTAL_PLATE; break;

    case ARM_FIRE_DRAGON_ARMOUR:    tile = TILEP_BODY_DRAGONARM_RED; break;
    case ARM_ICE_DRAGON_ARMOUR:     tile = TILEP_BODY_DRAGONARM_CYAN; break;
    case ARM_STEAM_DRAGON_ARMOUR:   tile = TILEP_BODY_DRAGONARM_WHITE; break;
    case ARM_ACID_DRAGON_ARMOUR:    tile = TILEP_BODY_DRAGONARM_YELLOW; break;
    case ARM_QUICKSILVER_DRAGON_ARMOUR: tile = TILEP_BODY_DRAGONARM_QUICKSILVER; break;
    case ARM_STORM_DRAGON_ARMOUR:   tile = TILEP_BODY_DRAGONARM_BLUE; break;
    case ARM_SHADOW_DRAGON_ARMOUR:  tile = TILEP_BODY_DRAGONARM_SHADOW; break;
    case ARM_GOLD_DRAGON_ARMOUR:    tile = TILEP_BODY_DRAGONARM_GOLD; break;
    case ARM_SWAMP_DRAGON_ARMOUR:   tile = TILEP_BODY_DRAGONARM_BROWN; break;
    case ARM_PEARL_DRAGON_ARMOUR:   tile = TILEP_BODY_DRAGONARM_PEARL; break;

    case ARM_ANIMAL_SKIN:           tile = TILEP_BODY_ANIMAL_SKIN; break;
    case ARM_TROLL_LEATHER_ARMOUR:  tile = TILEP_BODY_TROLL_LEATHER; break;
    case ARM_DEEP_TROLL_LEATHER_ARMOUR: tile = TILEP_BODY_DEEP_TROLL_LEATHER; break;
    case ARM_IRON_TROLL_LEATHER_ARMOUR: tile = TILEP_BODY_IRON_TROLL_LEATHER; break;
    case ARM_SALAMANDER_HIDE_ARMOUR: tile = TILEP_BODY_SALAMANDER_HIDE; break;

    default:                        tile = 0;
    }

    return tileidx_enchant_equ(item, tile, true);
}

tileidx_t tilep_equ_cloak(const item_def &item)
{
    if (item.base_type != OBJ_ARMOURS)
        return 0;

    if (item.props.exists("worn_tile"))
        return item.props["worn_tile"].get_short();

    if (is_unrandom_artefact(item))
    {
        const tileidx_t tile = unrandart_to_doll_tile(find_unrandart_index(item));
        if (tile)
            return tile;
    }

    switch (item.sub_type)
    {
        case ARM_CLOAK:
            return _modrng(item.rnd, TILEP_CLOAK_FIRST_NORM,
                           TILEP_CLOAK_LAST_NORM);

        case ARM_SCARF:
            return _modrng(item.rnd, TILEP_CLOAK_SCARF_FIRST_NORM,
                           TILEP_CLOAK_SCARF_LAST_NORM);
    }

    return 0;
}

tileidx_t tilep_equ_helm(const item_def &item)
{
    if (item.base_type != OBJ_ARMOURS)
        return 0;

    if (item.props.exists("worn_tile"))
        return item.props["worn_tile"].get_short();

    if (is_unrandom_artefact(item))
    {
        const tileidx_t tile = unrandart_to_doll_tile(find_unrandart_index(item));
        if (tile)
            return tile;

        // Although there shouldn't be any, just in case
        // unhandled artefacts fall through to defaults...
    }

    switch (item.sub_type)
    {
        case ARM_SKULL:
            return TILEP_HELM_DRAGON_SKULL;
        case ARM_CAP:
        {
            tileidx_t tile = TILEP_HELM_CAP;
            if (is_artefact(item))
                return tile + 2;
            if (get_armour_ego_type(item) != SPARM_NORMAL)
                return tile + 1;
            return tile;
        }
        case ARM_HAT:
            return _modrng(item.rnd, TILEP_HELM_HAT_FIRST_NORM,
                           TILEP_HELM_HAT_LAST_NORM);

        case ARM_HELMET:
            return _modrng(item.rnd, TILEP_HELM_FIRST_NORM,
                           TILEP_HELM_LAST_NORM);
    }

    return 0;
}

tileidx_t tilep_equ_gloves(const item_def &item)
{
    if (item.base_type != OBJ_ARMOURS)
        return 0;

    if (item.props.exists("worn_tile"))
        return item.props["worn_tile"].get_short();

    if (is_unrandom_artefact(item))
    {
        const tileidx_t tile = unrandart_to_doll_tile(find_unrandart_index(item));
        if (tile)
            return tile;
    }

    if (item.sub_type == ARM_GLOVES)
        return _modrng(item.rnd, TILEP_ARM_FIRST_NORM, TILEP_ARM_LAST_NORM);
    else if (item.sub_type == ARM_CLAW)
        return TILEP_ARM_CLAWS;
    return 0;
}

tileidx_t tilep_equ_boots(const item_def &item)
{
    if (item.base_type != OBJ_ARMOURS)
        return 0;

    if (item.props.exists("worn_tile"))
        return item.props["worn_tile"].get_short();

    int etype = enchant_to_int(item);

    if (is_unrandom_artefact(item))
    {
        const tileidx_t tile = unrandart_to_doll_tile(find_unrandart_index(item));
        if (tile)
            return tile;
    }

    if (item.sub_type == ARM_NAGA_BARDING)
        return TILEP_BOOTS_NAGA_BARDING + min(etype, 3);

    if (item.sub_type == ARM_CENTAUR_BARDING)
        return TILEP_BOOTS_CENTAUR_BARDING + min(etype, 3);

    if (item.sub_type != ARM_BOOTS)
        return 0;

    return _modrng(item.rnd, TILEP_BOOTS_FIRST_NORM, TILEP_BOOTS_LAST_NORM);
}

tileidx_t tileidx_player()
{
    tileidx_t ch = TILEP_PLAYER;

    // Handle shapechange first
    switch (you.form)
    {
    // equipment-using forms are handled regularly
    case transformation::statue:
    case transformation::lich:
    case transformation::tree:
        break;
    // animals
    case transformation::bat:
        if (you.get_mutation_level(MUT_INSUBSTANTIAL) == 1)
            ch = TILEP_TRAN_BAT_SPECTRAL;
        else    
            ch = TILEP_TRAN_BAT;       
        break;
    case transformation::scorpion:
        if (you.get_mutation_level(MUT_INSUBSTANTIAL) == 1)
            ch = TILEP_TRAN_SCORPION_SPECTRAL;
        else
            ch = TILEP_TRAN_SCORPION;
        break;
    case transformation::pig:
        if (you.get_mutation_level(MUT_INSUBSTANTIAL) == 1)
            ch = TILEP_TRAN_PIG_SPECTRAL;
        else
            ch = TILEP_TRAN_PIG;
        break;
#if TAG_MAJOR_VERSION == 34
    case transformation::porcupine: ch = TILEP_MONS_PORCUPINE; break;
#endif
    // non-animals
    case transformation::ice_beast:
        if (you.get_mutation_level(MUT_INSUBSTANTIAL) == 1)
            ch = TILEP_TRAN_ICE_BEAST_SPECTRAL;
        else
            ch = TILEP_TRAN_ICE_BEAST;
        break;
    case transformation::wisp:      ch = TILEP_MONS_INSUBSTANTIAL_WISP; break;
#if TAG_MAJOR_VERSION == 34
    case transformation::jelly:     ch = TILEP_MONS_JELLY;     break;
#endif
    case transformation::fungus:
        if (you.get_mutation_level(MUT_INSUBSTANTIAL) == 1)
            ch = TILEP_TRAN_MUSHROOM_SPECTRAL;
        else
            ch = TILEP_TRAN_MUSHROOM;
        break;
    case transformation::shadow:    ch = TILEP_TRAN_SHADOW;    break;
    case transformation::hydra:     
        if (you.get_mutation_level(MUT_INSUBSTANTIAL) == 1)
            ch = tileidx_mon_clamp(TILEP_TRAN_SPECTRAL_HYDRA, you.heads() - 1);
        else
            ch = tileidx_mon_clamp(TILEP_MONS_HYDRA, you.heads() - 1);
        break;
    case transformation::dragon:
    {
        if (you.undead_state() == US_GHOST)
            ch = TILEP_TRAN_DRAGON_SPECTRAL;
        else if (you.species == SP_DRACONIAN)
        {
            switch (you.drac_colour)
            {
            case DR_PLATINUM:           ch = TILEP_TRAN_DRAGON_PLATINUM;        break;
            case DR_BLOOD:              ch = TILEP_TRAN_DRAGON_BLOOD;           break;
            case DR_SCINTILLATING:      ch = TILEP_TRAN_DRAGON_SCINTILLATING;   break;
            case DR_PEARL:              ch = TILEP_TRAN_DRAGON_PEARL;           break;
            case DR_GOLDEN:             ch = TILEP_TRAN_DRAGON_GOLDEN;          break;
            case DR_BONE:               ch = TILEP_TRAN_DRAGON_BONE;            break;
            case DR_OLIVE:              ch = TILEP_TRAN_DRAGON_OLIVE;           break;
            case DR_MAGENTA:            ch = TILEP_TRAN_DRAGON_MAGENTA;         break;
            case DR_PINK:               ch = TILEP_TRAN_DRAGON_PINK;            break;
            case DR_BLUE:               ch = TILEP_TRAN_DRAGON_BLUE;            break;
            case DR_BLACK:              ch = TILEP_TRAN_DRAGON_BLACK;           break;
            case DR_LIME:               ch = TILEP_TRAN_DRAGON_LIME;            break;
            case DR_SILVER:             ch = TILEP_TRAN_DRAGON_SILVER;          break;
            case DR_GREEN:              ch = TILEP_TRAN_DRAGON_GREEN;           break;
            case DR_CYAN:               ch = TILEP_TRAN_DRAGON_CYAN;            break;
            case DR_PURPLE:             ch = TILEP_TRAN_DRAGON_PURPLE;          break;
            case DR_WHITE:              ch = TILEP_TRAN_DRAGON_WHITE;           break;
            case DR_RED:                ch = TILEP_TRAN_DRAGON_RED;             break;
            default:   /* DR_BROWN */   ch = TILEP_TRAN_DRAGON;                 break;
            }
        }
        else
            ch = TILEP_TRAN_DRAGON;
        break;
    }
    // no special tile
    case transformation::blade_hands:
    case transformation::appendage:
    case transformation::none:
    default:
        break;
    }

    // Currently, the flying flag is only used for not drawing the tile in the
    // water. in_water() checks Beogh's water walking. If the flying flag is
    // used for something else, we would need to add an in_water flag.
    if (!you.in_water())
        ch |= TILE_FLAG_FLYING;

    if (you.attribute[ATTR_HELD])
    {
        if (get_trapping_net(you.pos()) == NON_ITEM)
            ch |= TILE_FLAG_WEB;
        else
            ch |= TILE_FLAG_NET;
    }

    if (you.duration[DUR_POISONING])
    {
        int pois_perc = (you.hp <= 0) ? 100
                                  : ((you.hp - max(0, poison_survival())) * 100 / you.hp);
        if (pois_perc >= 100)
            ch |= TILE_FLAG_MAX_POISON;
        else if (pois_perc >= 35)
            ch |= TILE_FLAG_MORE_POISON;
        else
            ch |= TILE_FLAG_POISON;
    }

    return ch;
}

bool is_player_tile(tileidx_t tile, tileidx_t base_tile)
{
    return tile >= base_tile
           && tile < base_tile + tile_player_count(base_tile);
}

bool is_naga(tileidx_t tile)
{
    return (is_player_tile(tile, TILEP_BOTTOM_NAGA)
         || is_player_tile(tile, TILEP_BOTTOM_NAGA_LICH)
         || is_player_tile(tile, TILEP_BOTTOM_NAGA_STATUE)
         || is_player_tile(tile, TILEP_BOTTOM_NAGA_UNDEAD)
         || is_player_tile(tile, TILEP_BOTTOM_NAGA_SPECTRAL));
}

bool is_cent(tileidx_t tile)
{
    return (is_player_tile(tile, TILEP_BOTTOM_CENTAUR)
         || is_player_tile(tile, TILEP_BOTTOM_CENTAUR_BONE)
         || is_player_tile(tile, TILEP_BOTTOM_CENTAUR_STATUE)
         || is_player_tile(tile, TILEP_BOTTOM_CENTAUR_UNDEAD)
         || is_player_tile(tile, TILEP_BOTTOM_CENTAUR_SPECTRAL)
         || is_player_tile(tile, TILEP_BOTTOM_HIPPOGRIFF)
         || is_player_tile(tile, TILEP_BOTTOM_HIPPOGRIFF_LICH)
         || is_player_tile(tile, TILEP_BOTTOM_HIPPOGRIFF_STATUE));
}

bool is_merfolk_tail(tileidx_t tile)
{
    return (is_player_tile(tile, TILEP_BOTTOM_MERFOLK_WATER)
         || is_player_tile(tile, TILEP_BOTTOM_MERFOLK_WATER_STATUE)
         || is_player_tile(tile, TILEP_BOTTOM_MERFOLK_WATER_BONE)
         || is_player_tile(tile, TILEP_BOTTOM_MERFOLK_WATER_SPECTRAL)
         || is_player_tile(tile, TILEP_BOTTOM_MERFOLK_WATER_UNDEAD));
}

static int _draconian_colour(int colour)
{
    switch (colour)
    {
    case DR_BROWN: 
        if (you.char_class == JOB_MUMMY)    return 21; 
    default:                return 0;
    case DR_BLACK:          return 1;
    case DR_LIME:           return 2;
    case DR_GREEN:          return 3;
    case DR_CYAN:           return 4;
    case DR_PURPLE:         return 5;
    case DR_RED:            return 6;
    case DR_WHITE:          return 7;
    case DR_BLOOD:          return 8;
    case DR_BLUE:           return 9;
    case DR_BONE:           return 10;
    case DR_GOLDEN:         return 11;
    case DR_MAGENTA:        return 12;
    case DR_OLIVE:          return 13;
    case DR_PEARL:          return 14;
    case DR_PINK:           return 15;
    case DR_PLATINUM:       return 16;
    case DR_SCINTILLATING:  return 17;
    case DR_SILVER:         return 18;
    case DR_TEAL:           return 19;
    }
    return 0;
}

// Uses the top half of the player tile to pick the bottom half.
// Overrides with a special bottom half when appropriate (Merfolk in Water, Centaurs, etc.)
tileidx_t tilep_top_to_bottom_tile(tileidx_t top)
{
    if (you.fishtail)
    {
        if (you.undead_state() == US_GHOST)
            return TILEP_BOTTOM_MERFOLK_WATER_SPECTRAL;
        else if (you.species == SP_DRACONIAN && you.drac_colour == DR_BONE)
            return TILEP_BOTTOM_MERFOLK_WATER_BONE;
        else if (you.undead_state() != US_ALIVE)
            return TILEP_BOTTOM_MERFOLK_WATER_UNDEAD;
        return TILEP_BOTTOM_MERFOLK_WATER;
    }

    else if (you.species == SP_CENTAUR || you.char_class == JOB_CENTAUR)
    {
        if (you.undead_state() == US_GHOST)
            return TILEP_BOTTOM_CENTAUR_SPECTRAL;
        else if (you.species == SP_DRACONIAN && you.drac_colour == DR_BONE)
            return TILEP_BOTTOM_CENTAUR_BONE;
        else if (you.get_mutation_level(MUT_TALONS))
            return TILEP_BOTTOM_HIPPOGRIFF;
        else if (you.undead_state() != US_ALIVE)
            return TILEP_BOTTOM_CENTAUR_UNDEAD;
        return TILEP_BOTTOM_CENTAUR;
    }

    else if (you.species == SP_NAGA || you.char_class == JOB_NAGA)
    {
        if (you.undead_state() == US_GHOST)
            return TILEP_BOTTOM_NAGA_SPECTRAL;
        else if (you.species == SP_DRACONIAN && you.drac_colour == DR_BONE)
            return TILEP_BOTTOM_NAGA_LICH;
        else if (you.undead_state() != US_ALIVE)
            return TILEP_BOTTOM_NAGA_UNDEAD;
        else if (you.species == SP_OCTOPODE)
            return TILEP_BOTTOM_NAGA + 2;
        return TILEP_BOTTOM_NAGA;
    }

    if (top >= TILEP_BASE_BOTTOMLESS)
        return 0;

    const int offset = tile_player_part_start[TILEP_PART_BOTTOM] - tile_player_part_start[TILEP_PART_BASE];
    return top + offset;
}

tileidx_t tilep_species_to_base_tile(int sp, int drac_colour)
{
    if (sp != SP_OCTOPODE && sp != SP_FELID
        && sp != SP_FAIRY)
    {
        if (you.get_mutation_level(MUT_SLIME) >= 3)
            return TILEP_BASE_JIYVITE;
        if (you.char_class == JOB_MUMMY)
            return TILEP_BASE_MUMMY;
        if (you.char_class == JOB_DEMIGOD)
            return TILEP_BASE_DEMIGOD;
        if (you.char_class == JOB_DEMONSPAWN)
            return TILEP_BASE_DEMONSPAWN;
        if (you.char_class == JOB_VINE_STALKER)
            return TILEP_BASE_VINE_STALKER;
    }
    switch (sp)
    {
    case SP_HUMAN:
        return TILEP_BASE_HUMAN;
    case SP_DEEP_ELF:
        return TILEP_BASE_DEEP_ELF;
    case SP_HALFLING:
        return TILEP_BASE_HALFLING;
    case SP_HILL_ORC:
        return TILEP_BASE_ORC;
    case SP_KOBOLD:
        return TILEP_BASE_KOBOLD;
    case SP_MUMMY:
        return TILEP_BASE_MUMMY;
    case SP_NAGA:
        return TILEP_BASE_HUMAN;
    case SP_OGRE:
        return TILEP_BASE_OGRE;
    case SP_TROLL:
        return TILEP_BASE_TROLL;
    case SP_DRACONIAN:
    {
        const int colour_offset = _draconian_colour(drac_colour);
        return TILEP_BASE_DRACONIAN + colour_offset * 2;
    }
    case SP_CENTAUR:
        return TILEP_BASE_HUMAN;
    case SP_DEMIGOD:
        return TILEP_BASE_DEMIGOD;
    case SP_SPRIGGAN:
        return TILEP_BASE_SPRIGGAN;
    case SP_MINOTAUR:
        return TILEP_BASE_MINOTAUR;
    case SP_DEMONSPAWN:
        return TILEP_BASE_DEMONSPAWN;
    case SP_FAIRY:
        return TILEP_BASE_FAIRY;
    case SP_GHOUL:
        return TILEP_BASE_GHOUL;
    case SP_TENGU:
        return TILEP_BASE_TENGU;
    case SP_MERFOLK:
        return TILEP_BASE_MERFOLK;
    case SP_VAMPIRE:
        return TILEP_BASE_VAMPIRE;
    case SP_DEEP_DWARF:
        return TILEP_BASE_DEEP_DWARF;
    case SP_GARGOYLE:
        return TILEP_BASE_GARGOYLE;
    case SP_MOLTEN_GARGOYLE:
        return TILEP_BASE_MOLTEN_GARGOYLE;
    case SP_OOZOMORPH:
        return TILEP_BASE_OOZOMORPH;
    case SP_FELID:
        return TILEP_BASE_FELID;
    case SP_LIGNIFITE:
        return TILEP_BASE_LIGNIFITE;
    case SP_OCTOPODE:
        if (you.char_class == JOB_CENTAUR)
            return TILEP_BASE_OCTOPODE_CENTAUR;
        if (you.char_class == JOB_NAGA)
            return TILEP_BASE_OCTOPODE_NAGA;
        return TILEP_BASE_OCTOPODE;
    case SP_FORMICID:
        return TILEP_BASE_FORMICID;
    case SP_VINE_STALKER:
        return TILEP_BASE_VINE_STALKER;
    case SP_BARACHI:
        return TILEP_BASE_BARACHI;
    case SP_GNOLL:
        return TILEP_BASE_GNOLL;
    case SP_SILENT_SPECTRE:
        return TILEP_BASE_SILENT_SPECTRE;
    case SP_GOBLIN:
        return TILEP_BASE_GOBLIN;
    default:
        return TILEP_BASE_HUMAN;
    }
}

void tilep_draconian_init(int colour, tileidx_t *base,
                          tileidx_t *head, tileidx_t *wing)
{
    const int colour_offset = _draconian_colour(colour);
    *base = TILEP_BASE_DRACONIAN + colour_offset * 2;
    if (you.char_class == JOB_MUMMY && you.drac_colour == DR_BROWN)
        *base = TILEP_BASE_MUMMY;
    *head = tile_player_part_start[TILEP_PART_DRCHEAD] + colour_offset;

    if (you.has_mutation(MUT_BIG_WINGS))
        *wing = tile_player_part_start[TILEP_PART_DRCWING] + colour_offset;
}

// Set default parts of each race: body + optional beard, hair, etc.
// This function needs to be entirely deterministic.
void tilep_race_default(int sp, int colour, dolls_data *doll)
{
    tileidx_t *parts = doll->parts;

    tileidx_t result = tilep_species_to_base_tile(sp, colour);
    if (parts[TILEP_PART_BASE] != TILEP_SHOW_EQUIP)
        result = parts[TILEP_PART_BASE];

    tileidx_t hair   = 0;
    tileidx_t beard  = 0;
    tileidx_t wing   = 0;
    tileidx_t head   = 0;

    hair = TILEP_HAIR_SHORT_BLACK;

    switch (sp)
    {
        case SP_DEEP_ELF:
            hair = TILEP_HAIR_ELF_WHITE;
            break;
        case SP_TROLL:
            hair = TILEP_HAIR_TROLL;
            break;
        case SP_DRACONIAN:
        {
            tilep_draconian_init(colour, &result, &head, &wing);
            hair   = 0;
            break;
        }
        case SP_MERFOLK:
            result = TILEP_BASE_MERFOLK;
            hair = TILEP_HAIR_GREEN;
            break;
        case SP_NAGA:
            hair = TILEP_HAIR_PART2_RED;
            break;
        case SP_VAMPIRE:
            hair = TILEP_HAIR_ARWEN;
            break;
        case SP_DEEP_DWARF:
            hair  = TILEP_HAIR_SHORT_WHITE;
            beard = TILEP_BEARD_GARIBALDI_WHITE;
            break;
        case SP_SPRIGGAN:
            hair = 0;
            beard = TILEP_BEARD_MEDIUM_GREEN;
            break;
        case SP_MINOTAUR:
        case SP_DEMONSPAWN:
        case SP_GHOUL:
        case SP_HILL_ORC:
        case SP_KOBOLD:
        case SP_MUMMY:
        case SP_FORMICID:
        case SP_BARACHI:
        case SP_GNOLL:
        case SP_GARGOYLE:
        case SP_MOLTEN_GARGOYLE:
        case SP_VINE_STALKER:
        case SP_OOZOMORPH:
            hair = 0;
            break;
        default:
            // nothing to do
            break;
    }

    parts[TILEP_PART_BASE] = result;

    // Don't overwrite doll parts defined elsewhere.
    if (parts[TILEP_PART_HAIR] == TILEP_SHOW_EQUIP)
        parts[TILEP_PART_HAIR] = hair;
    if (parts[TILEP_PART_BEARD] == TILEP_SHOW_EQUIP)
        parts[TILEP_PART_BEARD] = beard;
    if (parts[TILEP_PART_SHADOW] == TILEP_SHOW_EQUIP)
        parts[TILEP_PART_SHADOW] = TILEP_SHADOW_SHADOW;
    if (parts[TILEP_PART_DRCHEAD] == TILEP_SHOW_EQUIP)
        parts[TILEP_PART_DRCHEAD] = head;
    if (parts[TILEP_PART_DRCWING] == TILEP_SHOW_EQUIP)
        parts[TILEP_PART_DRCWING] = wing;
}

// This function needs to be entirely deterministic.
void tilep_job_default(int job, dolls_data *doll)
{
    tileidx_t *parts = doll->parts;

    parts[TILEP_PART_CLOAK] = TILEP_SHOW_EQUIP;
    parts[TILEP_PART_BOOTS] = TILEP_SHOW_EQUIP;
    parts[TILEP_PART_LEG]   = TILEP_SHOW_EQUIP;
    parts[TILEP_PART_BODY]  = TILEP_SHOW_EQUIP;
    parts[TILEP_PART_ARM]   = TILEP_SHOW_EQUIP;
    parts[TILEP_PART_HAND1] = TILEP_SHOW_EQUIP;
    parts[TILEP_PART_HAND2] = TILEP_SHOW_EQUIP;
    parts[TILEP_PART_HELM]  = TILEP_SHOW_EQUIP;

    switch (job)
    {
        case JOB_FIGHTER:
            parts[TILEP_PART_LEG]   = TILEP_LEG_METAL_SILVER;
            break;

        case JOB_SKALD:
            parts[TILEP_PART_BODY]  = TILEP_BODY_SHIRT_WHITE3;
            parts[TILEP_PART_LEG]   = TILEP_LEG_SKIRT_OFS;
            parts[TILEP_PART_HELM]  = TILEP_HELM_HELM_IRON;
            parts[TILEP_PART_ARM]   = TILEP_ARM_GLOVE_GRAY;
            parts[TILEP_PART_BOOTS] = TILEP_BOOTS_MIDDLE_GRAY;
            parts[TILEP_PART_CLOAK] = TILEP_CLOAK_BLUE;
            break;

        case JOB_CHAOS_KNIGHT:
            parts[TILEP_PART_BODY]  = TILEP_BODY_MESH_BLACK;
            parts[TILEP_PART_LEG]   = TILEP_LEG_PANTS_SHORT_DARKBROWN;
            parts[TILEP_PART_HELM]  = TILEP_HELM_CLOWN; // Xom
            break;

        case JOB_ABYSSAL_KNIGHT:
            parts[TILEP_PART_BODY]  = TILEP_BODY_SHOULDER_PAD;
            parts[TILEP_PART_LEG]   = TILEP_LEG_METAL_GRAY;
            parts[TILEP_PART_HELM]  = TILEP_HELM_FHELM_PLUME;
            break;

        case JOB_BERSERKER:
            parts[TILEP_PART_BODY]  = TILEP_BODY_ANIMAL_SKIN;
            parts[TILEP_PART_LEG]   = TILEP_LEG_BELT_REDBROWN;
            break;

#if TAG_MAJOR_VERSION == 34
        case JOB_STALKER:
            parts[TILEP_PART_HELM]  = TILEP_HELM_HOOD_GREEN;
            parts[TILEP_PART_BODY]  = TILEP_BODY_LEATHER_JACKET;
            parts[TILEP_PART_LEG]   = TILEP_LEG_PANTS_SHORT_GRAY;
            parts[TILEP_PART_HAND1] = TILEP_HAND1_SWORD_THIEF;
            parts[TILEP_PART_HAND2] = TILEP_HAND2_BOOK_GREEN_DIM;
            parts[TILEP_PART_ARM]   = TILEP_ARM_GLOVE_WRIST_PURPLE;
            parts[TILEP_PART_CLOAK] = TILEP_CLOAK_GREEN;
            parts[TILEP_PART_BOOTS] = TILEP_BOOTS_MIDDLE_BROWN2;
            break;

        case JOB_ASSASSIN:
            parts[TILEP_PART_HELM]  = TILEP_HELM_MASK_NINJA_BLACK;
            parts[TILEP_PART_BODY]  = TILEP_BODY_SHIRT_BLACK3;
            parts[TILEP_PART_LEG]   = TILEP_LEG_PANTS_BLACK;
            parts[TILEP_PART_HAND1] = TILEP_HAND1_SWORD_THIEF;
            parts[TILEP_PART_ARM]   = TILEP_ARM_GLOVE_BLACK;
            parts[TILEP_PART_CLOAK] = TILEP_CLOAK_BLACK;
            parts[TILEP_PART_BOOTS] = TILEP_BOOTS_SHORT_BROWN2;
            break;
#endif

        case JOB_WIZARD:
            parts[TILEP_PART_BODY]  = TILEP_BODY_GANDALF_G;
            parts[TILEP_PART_HAND1] = TILEP_HAND1_GANDALF;
            parts[TILEP_PART_HAND2] = TILEP_HAND2_BOOK_CYAN_DIM;
            parts[TILEP_PART_BOOTS] = TILEP_BOOTS_SHORT_BROWN;
            parts[TILEP_PART_HELM]  = TILEP_HELM_WIZARD_GRAY;
            break;

#if TAG_MAJOR_VERSION == 34
        case JOB_HEALER:
            parts[TILEP_PART_BODY]  = TILEP_BODY_ROBE_WHITE;
            parts[TILEP_PART_ARM]   = TILEP_ARM_GLOVE_WHITE;
            parts[TILEP_PART_HAND1] = TILEP_HAND1_DAGGER;
            parts[TILEP_PART_BOOTS] = TILEP_BOOTS_SHORT_BROWN;
            parts[TILEP_PART_HELM]  = TILEP_HELM_FHELM_HEALER;
            break;
#endif

        case JOB_NOBLE:
            parts[TILEP_PART_BODY] = TILEP_BODY_ROBE_WHITE;

        case JOB_NECROMANCER:
            parts[TILEP_PART_BODY]  = TILEP_BODY_ROBE_BLACK;
            parts[TILEP_PART_HAND1] = TILEP_HAND1_STAFF_SKULL;
            parts[TILEP_PART_HAND2] = TILEP_HAND2_BOOK_BLACK;
            parts[TILEP_PART_BOOTS] = TILEP_BOOTS_SHORT_BROWN;
            break;

        case JOB_FIRE_ELEMENTALIST:
            parts[TILEP_PART_BODY]  = TILEP_BODY_ROBE_RED;
            parts[TILEP_PART_HAND1] = TILEP_HAND1_GANDALF;
            parts[TILEP_PART_HAND2] = TILEP_HAND2_BOOK_RED_DIM;
            parts[TILEP_PART_BOOTS] = TILEP_BOOTS_SHORT_BROWN;
            break;

        case JOB_ICE_ELEMENTALIST:
            parts[TILEP_PART_BODY]  = TILEP_BODY_ROBE_BLUE;
            parts[TILEP_PART_HAND1] = TILEP_HAND1_GANDALF;
            parts[TILEP_PART_HAND2] = TILEP_HAND2_BOOK_BLUE_DIM;
            parts[TILEP_PART_BOOTS] = TILEP_BOOTS_SHORT_BROWN;
            break;

        case JOB_AIR_ELEMENTALIST:
            parts[TILEP_PART_BODY]  = TILEP_BODY_ROBE_CYAN;
            parts[TILEP_PART_HAND1] = TILEP_HAND1_GANDALF;
            parts[TILEP_PART_HAND2] = TILEP_HAND2_BOOK_CYAN_DIM;
            parts[TILEP_PART_BOOTS] = TILEP_BOOTS_SHORT_BROWN;
            break;

        case JOB_EARTH_ELEMENTALIST:
            parts[TILEP_PART_BODY]  = TILEP_BODY_ROBE_YELLOW;
            parts[TILEP_PART_HAND1] = TILEP_HAND1_GANDALF;
            parts[TILEP_PART_HAND2] = TILEP_HAND2_BOOK_YELLOW_DIM;
            parts[TILEP_PART_BOOTS] = TILEP_BOOTS_SHORT_BROWN;
            break;

        case JOB_VENOM_MAGE:
            parts[TILEP_PART_BODY]  = TILEP_BODY_ROBE_GREEN;
            parts[TILEP_PART_HAND1] = TILEP_HAND1_GANDALF;
            parts[TILEP_PART_HAND2] = TILEP_HAND2_BOOK_GREEN_DIM;
            parts[TILEP_PART_BOOTS] = TILEP_BOOTS_SHORT_BROWN;
            break;

        case JOB_TRANSMUTER:
            parts[TILEP_PART_BODY]  = TILEP_BODY_ROBE_RAINBOW;
            parts[TILEP_PART_HAND1] = TILEP_HAND1_STAFF_RUBY;
            parts[TILEP_PART_HAND2] = TILEP_HAND2_BOOK_MAGENTA_DIM;
            parts[TILEP_PART_BOOTS] = TILEP_BOOTS_SHORT_BROWN;
            break;
#if TAG_MAJOR_VERSION == 34
        case JOB_CONJURER:
            parts[TILEP_PART_BODY]  = TILEP_BODY_ROBE_MAGENTA;
            parts[TILEP_PART_HELM]  = TILEP_HELM_WIZARD_GRAY;
            parts[TILEP_PART_HAND1] = TILEP_HAND1_STAFF_MAGE2;
            parts[TILEP_PART_HAND2] = TILEP_HAND2_BOOK_RED_DIM;
            parts[TILEP_PART_BOOTS] = TILEP_BOOTS_SHORT_BROWN;
            break;
#endif
        case JOB_ENCHANTER:
            parts[TILEP_PART_BODY]  = TILEP_BODY_ROBE_YELLOW;
            parts[TILEP_PART_HELM]  = TILEP_HELM_WIZARD_GRAY;
            parts[TILEP_PART_HAND1] = TILEP_HAND1_STAFF_MAGE;
            parts[TILEP_PART_HAND2] = TILEP_HAND2_BOOK_BLUE_DIM;
            parts[TILEP_PART_BOOTS] = TILEP_BOOTS_SHORT_BROWN;
            break;

        case JOB_SUMMONER:
            parts[TILEP_PART_BODY]  = TILEP_BODY_ROBE_BROWN;
            parts[TILEP_PART_HELM]  = TILEP_HELM_WIZARD_GRAY;
            parts[TILEP_PART_HAND1] = TILEP_HAND1_STAFF_RING_BLUE;
            parts[TILEP_PART_HAND2] = TILEP_HAND2_BOOK_YELLOW_DIM;
            parts[TILEP_PART_BOOTS] = TILEP_BOOTS_SHORT_BROWN;
            break;

        case JOB_WARPER:
            parts[TILEP_PART_BODY]  = TILEP_BODY_ROBE_BROWN;
            parts[TILEP_PART_HELM]  = TILEP_HELM_WIZARD_GRAY;
            parts[TILEP_PART_HAND1] = TILEP_HAND1_SARUMAN;
            parts[TILEP_PART_HAND2] = TILEP_HAND2_BOOK_WHITE;
            parts[TILEP_PART_BOOTS] = TILEP_BOOTS_SHORT_BROWN;
            parts[TILEP_PART_CLOAK] = TILEP_CLOAK_RED;
            break;

        case JOB_ARCANE_MARKSMAN:
            parts[TILEP_PART_BODY]  = TILEP_BODY_ROBE_BROWN;
            parts[TILEP_PART_HELM]  = TILEP_HELM_WIZARD_GRAY;
            parts[TILEP_PART_HAND1] = TILEP_HAND1_SARUMAN;
            parts[TILEP_PART_HAND2] = TILEP_HAND2_BOOK_WHITE;
            parts[TILEP_PART_BOOTS] = TILEP_BOOTS_SHORT_BROWN;
            parts[TILEP_PART_CLOAK] = TILEP_CLOAK_RED;
            break;

        case JOB_HUNTER:
            parts[TILEP_PART_BODY]  = TILEP_BODY_LEGOLAS;
            parts[TILEP_PART_HELM]  = TILEP_HELM_FEATHER_GREEN;
            parts[TILEP_PART_LEG]   = TILEP_LEG_PANTS_DARKGREEN;
            parts[TILEP_PART_BOOTS] = TILEP_BOOTS_MIDDLE_BROWN3;
            break;

        case JOB_GLADIATOR:
            parts[TILEP_PART_HAND2] = TILEP_HAND2_SHIELD_ROUND2;
            parts[TILEP_PART_BODY]  = TILEP_BODY_BELT1;
            parts[TILEP_PART_LEG]   = TILEP_LEG_BELT_GRAY;
            parts[TILEP_PART_BOOTS] = TILEP_BOOTS_MIDDLE_GRAY;
            break;

        case JOB_MONK:
        case JOB_PRIEST:
            parts[TILEP_PART_BODY]  = TILEP_BODY_MONK_BLACK;
            break;

        case JOB_WANDERER:
            parts[TILEP_PART_BODY]  = TILEP_BODY_SHIRT_HAWAII;
            parts[TILEP_PART_LEG]   = TILEP_LEG_PANTS_SHORT_BROWN;
            parts[TILEP_PART_BOOTS] = TILEP_BOOTS_MIDDLE_BROWN3;
            break;

        case JOB_ARTIFICER:
            parts[TILEP_PART_HAND1] = TILEP_HAND1_SCEPTRE;
            parts[TILEP_PART_BODY]  = TILEP_BODY_LEATHER_ARMOUR;
            parts[TILEP_PART_LEG]   = TILEP_LEG_PANTS_BLACK;
            break;
    }
}

void tilep_calc_flags(const dolls_data &doll, int flag[])
{
    for (unsigned i = 0; i < TILEP_PART_MAX; i++)
        flag[i] = TILEP_FLAG_NORMAL;

    if (doll.parts[TILEP_PART_HELM] >= TILEP_HELM_HELM_OFS)
        flag[TILEP_PART_HAIR] = TILEP_FLAG_HIDE;

    if (doll.parts[TILEP_PART_HELM] >= TILEP_HELM_FHELM_OFS)
        flag[TILEP_PART_BEARD] = TILEP_FLAG_HIDE;

    if (doll.parts[TILEP_PART_HELM] >= TILEP_HELM_HELM_OFS)
        flag[TILEP_PART_DRCHEAD] = TILEP_FLAG_HIDE;

    if (is_naga(doll.parts[TILEP_PART_BOTTOM]))
    {
        flag[TILEP_PART_BODY] = TILEP_FLAG_CUT_NAGA;
        flag[TILEP_PART_BOOTS] = flag[TILEP_PART_LEG]  = TILEP_FLAG_HIDE;
    }
    else if (is_cent(doll.parts[TILEP_PART_BOTTOM]))
    {
        flag[TILEP_PART_BODY] = TILEP_FLAG_CUT_CENTAUR;
        flag[TILEP_PART_BOOTS] = flag[TILEP_PART_LEG] = TILEP_FLAG_HIDE;
    }
    else if (is_merfolk_tail(doll.parts[TILEP_PART_BOTTOM]))
    {
        flag[TILEP_PART_BOOTS]  = TILEP_FLAG_HIDE;
        flag[TILEP_PART_LEG]    = TILEP_FLAG_HIDE;
        flag[TILEP_PART_SHADOW] = TILEP_FLAG_HIDE;
    }

    if (doll.parts[TILEP_PART_BASE] >= TILEP_BASE_DRACONIAN_FIRST
             && doll.parts[TILEP_PART_BASE] <= TILEP_BASE_DRACONIAN_LAST)
    {
        flag[TILEP_PART_HAIR] = TILEP_FLAG_HIDE;
    }
    else if (is_player_tile(doll.parts[TILEP_PART_BASE], TILEP_BASE_FELID))
    {
        flag[TILEP_PART_CLOAK] = TILEP_FLAG_HIDE;
        flag[TILEP_PART_BOOTS] = TILEP_FLAG_HIDE;
        flag[TILEP_PART_LEG]   = TILEP_FLAG_HIDE;
        flag[TILEP_PART_BODY]  = TILEP_FLAG_HIDE;
        flag[TILEP_PART_ARM]   = TILEP_FLAG_HIDE;
        if (!is_player_tile(doll.parts[TILEP_PART_HAND1],
                            TILEP_HAND1_BLADEHAND_FE))
        {
            flag[TILEP_PART_HAND1] = TILEP_FLAG_HIDE;
            flag[TILEP_PART_HAND2] = TILEP_FLAG_HIDE;
        }
        if (!is_player_tile(doll.parts[TILEP_PART_HELM], TILEP_HELM_HORNS_CAT))
            flag[TILEP_PART_HELM] = TILEP_FLAG_HIDE;
        flag[TILEP_PART_HAIR]  = TILEP_FLAG_HIDE;
        flag[TILEP_PART_BEARD] = TILEP_FLAG_HIDE;
        flag[TILEP_PART_SHADOW]= TILEP_FLAG_HIDE;
        flag[TILEP_PART_DRCWING]=TILEP_FLAG_HIDE;
        flag[TILEP_PART_DRCHEAD]=TILEP_FLAG_HIDE;
    }
    else if (is_player_tile(doll.parts[TILEP_PART_BASE], TILEP_BASE_FAIRY))
    {
        flag[TILEP_PART_CLOAK] = TILEP_FLAG_HIDE;
        flag[TILEP_PART_BOOTS] = TILEP_FLAG_HIDE;
        flag[TILEP_PART_LEG] = TILEP_FLAG_HIDE;
        flag[TILEP_PART_BODY] = TILEP_FLAG_HIDE;
        flag[TILEP_PART_ARM] = TILEP_FLAG_HIDE;
        flag[TILEP_PART_HAND1] = TILEP_FLAG_HIDE;
        flag[TILEP_PART_HAND2] = TILEP_FLAG_HIDE;
        flag[TILEP_PART_HELM] = TILEP_FLAG_HIDE;
        flag[TILEP_PART_HAIR] = TILEP_FLAG_HIDE;
        flag[TILEP_PART_BEARD] = TILEP_FLAG_HIDE;
        flag[TILEP_PART_SHADOW] = TILEP_FLAG_HIDE;
        flag[TILEP_PART_DRCWING] = TILEP_FLAG_HIDE;
        flag[TILEP_PART_DRCHEAD] = TILEP_FLAG_HIDE;
    }
    else if (is_player_tile(doll.parts[TILEP_PART_BASE], TILEP_BASE_OCTOPODE)
          || is_player_tile(doll.parts[TILEP_PART_BASE], TILEP_BASE_OCTOPODE_NAGA)
          || is_player_tile(doll.parts[TILEP_PART_BASE], TILEP_BASE_OCTOPODE_CENTAUR))
    {
        flag[TILEP_PART_CLOAK] = TILEP_FLAG_HIDE;
        flag[TILEP_PART_BOOTS] = TILEP_FLAG_HIDE;
        flag[TILEP_PART_LEG]   = TILEP_FLAG_HIDE;
        flag[TILEP_PART_BODY]  = TILEP_FLAG_HIDE;
        if (doll.parts[TILEP_PART_ARM] != TILEP_ARM_OCTOPODE_SPIKE
            || is_player_tile(doll.parts[TILEP_PART_BASE], TILEP_BASE_OCTOPODE_CENTAUR))
        {
            flag[TILEP_PART_ARM] = TILEP_FLAG_HIDE;
        }
        flag[TILEP_PART_HAIR]  = TILEP_FLAG_HIDE;
        flag[TILEP_PART_BEARD] = TILEP_FLAG_HIDE;
        flag[TILEP_PART_SHADOW]= TILEP_FLAG_HIDE;
        flag[TILEP_PART_DRCWING]=TILEP_FLAG_HIDE;
        flag[TILEP_PART_DRCHEAD]=TILEP_FLAG_HIDE;
    }
    else if (is_player_tile(doll.parts[TILEP_PART_BASE], TILEP_BASE_LIGNIFITE))
    {
        flag[TILEP_PART_BOOTS] = TILEP_FLAG_HIDE;
        flag[TILEP_PART_LEG] = TILEP_FLAG_HIDE;
        flag[TILEP_PART_HAIR] = TILEP_FLAG_HIDE;
        flag[TILEP_PART_BEARD] = TILEP_FLAG_HIDE;
    }

    if (doll.parts[TILEP_PART_ARM] == TILEP_ARM_OCTOPODE_SPIKE
        && !is_player_tile(doll.parts[TILEP_PART_BASE], TILEP_BASE_OCTOPODE))
    {
        flag[TILEP_PART_ARM] = TILEP_FLAG_HIDE;
    }
    if (is_player_tile(doll.parts[TILEP_PART_HELM], TILEP_HELM_HORNS_CAT)
        && (!is_player_tile(doll.parts[TILEP_PART_BASE],
                            TILEP_BASE_FELID)
            && (!is_player_tile(doll.parts[TILEP_PART_BASE],
                                TILEP_TRAN_STATUE_FELID)
            // Every felid tile has its own horns.
            || doll.parts[TILEP_PART_BASE] - TILEP_BASE_FELID
               != doll.parts[TILEP_PART_HELM] - TILEP_HELM_HORNS_CAT)))
    {
        flag[TILEP_PART_ARM] = TILEP_FLAG_HIDE;
    }
}

// Parts index to string
static void _tilep_part_to_str(int number, char *buf)
{
    //special
    if (number == TILEP_SHOW_EQUIP)
        buf[0] = buf[1] = buf[2] = '*';
    else
    {
        //normal 2 digits
        buf[0] = '0' + (number/100) % 10;
        buf[1] = '0' + (number/ 10) % 10;
        buf[2] = '0' +  number      % 10;
    }
    buf[3] = '\0';
}

// Parts string to index
static int _tilep_str_to_part(char *str)
{
    //special
    if (str[0] == '*')
        return TILEP_SHOW_EQUIP;

    //normal 3 digits
    return atoi(str);
}

// This order is to preserve dolls.txt integrity over multiple versions.
// Newer entries should be added to the end before the -1 terminator.
const int parts_saved[TILEP_PART_MAX + 1] =
{
    TILEP_PART_SHADOW,
    TILEP_PART_BASE,
    TILEP_PART_CLOAK,
    TILEP_PART_BOOTS,
    TILEP_PART_LEG,
    TILEP_PART_BODY,
    TILEP_PART_ARM,
    TILEP_PART_HAND1,
    TILEP_PART_HAND2,
    TILEP_PART_HAIR,
    TILEP_PART_BEARD,
    TILEP_PART_HELM,
    TILEP_PART_HALO,
    TILEP_PART_ENCH,
    TILEP_PART_DRCWING,
    TILEP_PART_DRCHEAD,
    TILEP_PART_MOUNT_FRONT,
    TILEP_PART_MOUNT_BACK,
    TILEP_PART_BOTTOM,
    -1
};

/*
 * scan input line from dolls.txt
 */
void tilep_scan_parts(char *fbuf, dolls_data &doll, int species, int colour)
{
    char  ibuf[8];

    int gcount = 0;
    int ccount = 0;
    for (int i = 0; parts_saved[i] != -1; ++i)
    {
        ccount = 0;
        int p = parts_saved[i];

        while (fbuf[gcount] != ':' && fbuf[gcount] != '\n'
               && ccount < 4 && gcount < (i+1)*4)
        {
            ibuf[ccount++] = fbuf[gcount++];
        }

        ibuf[ccount] = '\0';
        gcount++;

        const tileidx_t idx = _tilep_str_to_part(ibuf);
        if (idx == TILEP_SHOW_EQUIP)
            doll.parts[p] = TILEP_SHOW_EQUIP;
        else if (p == TILEP_PART_BASE)
        {
            const tileidx_t base_tile = tilep_species_to_base_tile(species, colour);
            if (idx >= tile_player_count(base_tile))
                doll.parts[p] = base_tile;
            else
                doll.parts[p] = base_tile + idx;
        }
        else if (idx == 0)
            doll.parts[p] = 0;
        else if (idx > tile_player_part_count[p])
            doll.parts[p] = tile_player_part_start[p];
        else
        {
            const tileidx_t idx2 = tile_player_part_start[p] + idx - 1;
            if (idx2 < TILE_MAIN_MAX || idx2 >= TILEP_PLAYER_MAX)
                doll.parts[p] = TILEP_SHOW_EQUIP;
            else
                doll.parts[p] = idx2;
        }
    }
}

/*
 * format-print parts
 */
void tilep_print_parts(char *fbuf, const dolls_data &doll)
{
    char *ptr = fbuf;
    for (unsigned i = 0; parts_saved[i] != -1; ++i)
    {
        const int p = parts_saved[i];
        tileidx_t idx = doll.parts[p];
        if (idx != TILEP_SHOW_EQUIP)
        {
            if (p == TILEP_PART_BASE)
                idx -= tilep_species_to_base_tile(you.species, you.drac_colour);
            else if (idx != 0)
            {
                idx = doll.parts[p] - tile_player_part_start[p] + 1;
                if (idx > tile_player_part_count[p])
                    idx = 0;
            }
        }
        _tilep_part_to_str(idx, ptr);

        ptr += 3;

        *ptr = ':';
        ptr++;
    }
    ptr[0] = '\n'; // erase the last ':'
    ptr[1] = 0;
}

#endif
