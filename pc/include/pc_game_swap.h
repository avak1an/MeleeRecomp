/**
 * @file pc_game_swap.h
 * Byte-swapping of the game's own (non-HSD) data structures loaded from
 * disc: stage, fighter and item tables. Implemented in pc/src/game_swap.c;
 * called from the game's loaders inside `#ifdef TARGET_PC` blocks.
 */
#ifndef PC_GAME_SWAP_H
#define PC_GAME_SWAP_H

#include <stddef.h>

#include <melee/gr/forward.h>
#include <melee/mp/forward.h>

/// Stage archive roots: map_head, grGroundParam, coll_data, itemdata (a
/// NULL-terminated array of {s32 kind; Article* article} pointers).
void pc_swap_stage_data(struct UnkStageDat* map_head, struct GroundParam* param,
                        struct MapCollData* coll, void** itemdata);

/// Fighter data root ("ftData" in Pl*.dat). anim_count / alt_anim_count are
/// the lengths of the xC and x14 animation tables (static per fighter).
struct ftData;
void pc_swap_ftdata(struct ftData* d, int anim_count, int alt_anim_count, int costumes);

/// Fighter common tables (PlCo.dat "ftLoadCommonData"), an array of 23 pointers.
void pc_swap_ft_common(void** tables);

/// Bone dynamics descriptor read from a stage or fighter archive.
struct DynamicsDesc;
void pc_swap_dynamics_desc(struct DynamicsDesc* desc);

/// Item data: one article (attributes, hurt boxes, model, dynamics) and the
/// ItCo.dat root holding the common, character and Pokemon article tables.
struct Article;
struct it_804D6D20_t;
void pc_swap_article(struct Article* article);
void pc_swap_stage_article(struct Article* article); ///< one article from a stage file
void pc_swap_item_common(struct it_804D6D20_t* root);

/// Byte-swap an animation command script in place (fighter subaction or
/// item state script), following subroutine and goto targets.
enum { PC_SCRIPT_FIGHTER, PC_SCRIPT_ITEM, PC_SCRIPT_OVERLAY };
void pc_swap_script(void* start, int kind);

/// Character-specific attribute block (ftData::ext_attr), size in bytes.
void pc_swap_ext_attrs(void* attrs, size_t size);

/// Fighter animation tree found in a figatree archive.
struct FigaTree;
void pc_swap_figatree(struct FigaTree* tree);

#endif
