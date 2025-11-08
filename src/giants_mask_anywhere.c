/*
 * Giant's Mask Anywhere Mod v1.0
 * Complete with camera, damage, and boots-based movement
 */

#include "modding.h"
#include "global.h"
#include <math.h>

extern void func_80855218(PlayState* play, Player* this, void* arg2);
extern void func_8085B384(Player* this, PlayState* play);
extern void func_80123140(PlayState* play, Player* player);

typedef enum {
    GMASK_CS_WAITING = 0,
    GMASK_CS_GROW = 1,
    GMASK_CS_GROW_SKIP = 2,
    GMASK_CS_SHRINK = 10,
    GMASK_CS_SHRINK_SKIP = 11,
    GMASK_CS_DONE = 20,
} GiantMaskCsState;

static u32 sCsTimer = 0;
static s16 sCsState = 0;
static s16 sSubCamId = 0;
static Vec3f sSubCamEye, sSubCamAt, sSubCamUp;
static f32 sSubCamUpRotZ = 0.0f;
static f32 sSubCamUpRotZScale = 0.0f;
static f32 sSubCamAtVel = 0.0f;
static f32 sSubCamDistZ = 0.0f;
static f32 sSubCamEyeOffsetY = 10.0f;
static f32 sSubCamAtOffsetY = 0.0f;
static f32 sSubCamAtTargetY = 0.0f;
static f32 sPlayerScale = 0.01f;
static bool sHasSeenGrow = false;
static bool sHasSeenShrink = false;
static bool sIsGiant = false;

// Scale age properties for movement speed
static void ScaleAgeProperties(Player* this, f32 scale) {
    this->ageProperties->ceilingCheckHeight *= scale;
    this->ageProperties->unk_0C *= scale;
    this->ageProperties->unk_10 *= scale;
    this->ageProperties->unk_14 *= scale;
    this->ageProperties->unk_18 *= scale;
    this->ageProperties->unk_1C *= scale;
    this->ageProperties->unk_24 *= scale;
    this->ageProperties->unk_28 *= scale;
    this->ageProperties->unk_2C *= scale;
    this->ageProperties->unk_30 *= scale;
    this->ageProperties->unk_34 *= scale;
    this->ageProperties->wallCheckRadius *= scale;
    this->ageProperties->unk_3C *= scale;
    this->ageProperties->unk_40 *= scale;
}

// CRITICAL: Hook Player_GetHeight - this makes camera work automatically!
RECOMP_PATCH f32 Player_GetHeight(Player* this) {
    f32 normalHeight;
    
    // Get base height based on form
    if (this->transformation == PLAYER_FORM_FIERCE_DEITY) {
        normalHeight = 87.0f;
    } else if (this->transformation == PLAYER_FORM_GORON) {
        normalHeight = 52.04f;
    } else if (this->transformation == PLAYER_FORM_ZORA) {
        normalHeight = 45.0f;
    } else if (this->transformation == PLAYER_FORM_DEKU) {
        normalHeight = 28.0f;
    } else {
        normalHeight = 44.0f;  // Human Link
    }
    
    // Multiply by 10 if giant - camera adjusts automatically!
    if (sIsGiant) {
        return normalHeight * 10.0f;
    }
    
    return normalHeight;
}

// Hook melee weapon damage (sword/punch attacks)
RECOMP_PATCH void func_80833728(Player* this, s32 index, u32 dmgFlags, s32 damage) {
    // Multiply damage by 10 when giant
    if (sIsGiant) {
        damage *= 10;
        // Add powder keg flag for breaking certain objects
        dmgFlags |= DMG_POWDER_KEG;
    }
    
    this->meleeWeaponQuads[index].elem.atDmgInfo.dmgFlags = dmgFlags;
    this->meleeWeaponQuads[index].elem.atDmgInfo.damage = damage;
    
    if (dmgFlags == DMG_DEKU_STICK) {
        this->meleeWeaponQuads[index].elem.atElemFlags = (ATELEM_ON | ATELEM_NEAREST | ATELEM_SFX_WOOD);
    } else {
        this->meleeWeaponQuads[index].elem.atElemFlags = (ATELEM_ON | ATELEM_NEAREST);
    }
}

// Hook cylinder attack damage (Goron pound, body attacks)
RECOMP_PATCH void Player_SetCylinderForAttack(Player* this, u32 dmgFlags, s32 damage, s32 radius) {
    // Multiply damage and radius when giant
    if (sIsGiant) {
        damage = damage * 10;
        radius = radius * 100;
        dmgFlags = dmgFlags | DMG_POWDER_KEG;
    }
    
    this->cylinder.base.atFlags = AT_ON | AT_TYPE_PLAYER;
    if (radius > 30) {
        this->cylinder.base.ocFlags1 = OC1_NONE;
    } else {
        this->cylinder.base.ocFlags1 = OC1_ON | OC1_TYPE_ALL;
    }
    
    this->cylinder.elem.atElemFlags = ATELEM_ON | ATELEM_NEAREST | ATELEM_SFX_NORMAL;
    this->cylinder.dim.radius = radius;
    this->cylinder.elem.atDmgInfo.dmgFlags = dmgFlags;
    this->cylinder.elem.atDmgInfo.damage = damage;
    
    if (dmgFlags & DMG_GORON_POUND) {
        this->cylinder.base.acFlags = AC_NONE;
    } else {
        this->cylinder.base.colMaterial = COL_MATERIAL_NONE;
        this->cylinder.elem.acDmgInfo.dmgFlags = 0xF7CFFFFF;
        
        if (dmgFlags & DMG_ZORA_BARRIER) {
            this->cylinder.base.acFlags = AC_NONE;
        } else {
            this->cylinder.base.acFlags = AC_ON | AC_TYPE_ENEMY;
        }
    }
}

// Hook Math3D_Vec3fDistSq - reduce distance for giant reach
RECOMP_PATCH f32 Math3D_Vec3fDistSq(Vec3f* a, Vec3f* b) {
    f32 dx = a->x - b->x;
    f32 dy = a->y - b->y;
    f32 dz = a->z - b->z;
    f32 distSq = (dx * dx) + (dy * dy) + (dz * dz);
    
    // If giant and within reasonable giant reach, return 0 (always hit)
    if (sIsGiant && distSq < (10000.0f * 10000.0f)) {
        return 0.0f;
    }
    
    return distSq;
}

static void HandleCutscene(Player* this, PlayState* play) {
    Vec3f eyeOffset;
    
    sCsTimer++;
    
    switch (sCsState) {
        case GMASK_CS_WAITING:
            if (this->stateFlags1 & PLAYER_STATE1_100) {
                sSubCamId = Play_CreateSubCamera(play);
                Play_ChangeCameraStatus(play, CAM_ID_MAIN, CAM_STATUS_WAIT);
                Play_ChangeCameraStatus(play, sSubCamId, CAM_STATUS_ACTIVE);
                sCsTimer = 0;
                sSubCamAtVel = 0.0f;
                sSubCamUpRotZScale = 0.0f;
                
                if (!sIsGiant) {
                    sCsState = GMASK_CS_GROW;
                    sSubCamDistZ = 60.0f;
                    sSubCamAtOffsetY = 23.0f;
                    sSubCamAtTargetY = 273.0f;
                    sPlayerScale = 0.01f;
                    goto grow;
                } else {
                    sCsState = GMASK_CS_SHRINK;
                    sSubCamDistZ = 200.0f;
                    sSubCamAtOffsetY = 273.0f;
                    sSubCamAtTargetY = 23.0f;
                    sPlayerScale = 0.1f;
                    goto shrink;
                }
            }
            break;
            
        case GMASK_CS_GROW:
            if (sCsTimer < 80 && sHasSeenGrow &&
                CHECK_BTN_ANY(CONTROLLER1(&play->state)->press.button,
                    BTN_A | BTN_B | BTN_CUP | BTN_CDOWN | BTN_CLEFT | BTN_CRIGHT)) {
                sCsState = GMASK_CS_GROW_SKIP;
                sCsTimer = 0;
                break;
            }
            
        grow:
            if (sCsTimer >= 50) {
                if (sCsTimer == 60) {
                    Audio_PlaySfx(NA_SE_PL_TRANSFORM_GIANT);
                }
                Math_ApproachF(&sSubCamDistZ, 200.0f, 0.1f, sSubCamAtVel * 640.0f);
                Math_ApproachF(&sSubCamAtOffsetY, sSubCamAtTargetY, 0.1f, sSubCamAtVel * 150.0f);
                Math_ApproachF(&sPlayerScale, 0.1f, 0.2f, sSubCamAtVel * 0.1f);
                Math_ApproachF(&sSubCamAtVel, 1.0f, 1.0f, 0.001f);
            } else {
                Math_ApproachF(&sSubCamDistZ, 30.0f, 0.1f, 1.0f);
            }
            
            if (sCsTimer > 50) {
                Math_ApproachZeroF(&sSubCamUpRotZScale, 1.0f, 0.06f);
            } else {
                Math_ApproachF(&sSubCamUpRotZScale, 0.4f, 1.0f, 0.02f);
            }
            
            if (sCsTimer > 120) {
                if (!sIsGiant) {
                    sIsGiant = true;
                    ScaleAgeProperties(this, 10.0f);
                    
                    // Set Giant Boots like Twinmold does!
                    this->currentBoots = PLAYER_BOOTS_GIANT;
                    func_80123140(play, this);
                }
                sHasSeenGrow = true;
                goto done;
            }
            break;
            
        case GMASK_CS_GROW_SKIP:
            if (sCsTimer >= 8) {
                if (!sIsGiant) {
                    sIsGiant = true;
                    ScaleAgeProperties(this, 10.0f);
                    
                    // Set Giant Boots like Twinmold does!
                    this->currentBoots = PLAYER_BOOTS_GIANT;
                    func_80123140(play, this);
                }
                sPlayerScale = 0.1f;
                goto done;
            }
            break;
            
        case GMASK_CS_SHRINK:
            if (sCsTimer < 30 && sHasSeenShrink &&
                CHECK_BTN_ANY(CONTROLLER1(&play->state)->press.button,
                    BTN_A | BTN_B | BTN_CUP | BTN_CDOWN | BTN_CLEFT | BTN_CRIGHT)) {
                sCsState = GMASK_CS_SHRINK_SKIP;
                sCsTimer = 0;
                break;
            }
            
        shrink:
            if (sCsTimer != 0) {
                if (sCsTimer == 10) {
                    Audio_PlaySfx(NA_SE_PL_TRANSFORM_NORAML);
                }
                Math_ApproachF(&sSubCamDistZ, 60.0f, 0.1f, sSubCamAtVel * 640.0f);
                Math_ApproachF(&sSubCamAtOffsetY, sSubCamAtTargetY, 0.1f, sSubCamAtVel * 150.0f);
                Math_ApproachF(&sPlayerScale, 0.01f, 0.1f, 0.003f);
                Math_ApproachF(&sSubCamAtVel, 2.0f, 1.0f, 0.01f);
            }
            
            if (sCsTimer > 50) {
                if (sIsGiant) {
                    sIsGiant = false;
                    ScaleAgeProperties(this, 0.1f);
                    
                    // Reset to normal boots
                    this->currentBoots = PLAYER_BOOTS_HYLIAN;
                    func_80123140(play, this);
                }
                sHasSeenShrink = true;
                goto done;
            }
            break;
            
        case GMASK_CS_SHRINK_SKIP:
            if (sCsTimer >= 8) {
                if (sIsGiant) {
                    sIsGiant = false;
                    ScaleAgeProperties(this, 0.1f);
                    
                    // Reset to normal boots
                    this->currentBoots = PLAYER_BOOTS_HYLIAN;
                    func_80123140(play, this);
                }
                sPlayerScale = 0.01f;
                goto done;
            }
            break;
            
        done:
        case GMASK_CS_DONE:
            sCsState = GMASK_CS_WAITING;
            func_80169AFC(play, sSubCamId, 0);
            sSubCamId = SUB_CAM_ID_DONE;
            this->stateFlags1 &= ~PLAYER_STATE1_100;
            break;
    }
    
    Actor_SetScale(&this->actor, sPlayerScale);
    
    // Cutscene camera
    if (sCsState != GMASK_CS_WAITING && sSubCamId != SUB_CAM_ID_DONE) {
        Matrix_RotateYS(this->actor.shape.rot.y, MTXMODE_NEW);
        Matrix_MultVecZ(sSubCamDistZ, &eyeOffset);
        
        sSubCamEye.x = this->actor.world.pos.x + eyeOffset.x;
        sSubCamEye.y = this->actor.world.pos.y + eyeOffset.y + sSubCamEyeOffsetY;
        sSubCamEye.z = this->actor.world.pos.z + eyeOffset.z;
        
        sSubCamAt.x = this->actor.world.pos.x;
        sSubCamAt.y = this->actor.world.pos.y + sSubCamAtOffsetY;
        sSubCamAt.z = this->actor.world.pos.z;
        
        sSubCamUpRotZ = Math_SinS(sCsTimer * 1512) * sSubCamUpRotZScale;
        Matrix_RotateZF(sSubCamUpRotZ, MTXMODE_APPLY);
        Matrix_MultVecY(1.0f, &sSubCamUp);
        Play_SetCameraAtEyeUp(play, sSubCamId, &sSubCamAt, &sSubCamEye, &sSubCamUp);
    }
}

RECOMP_HOOK("Player_UpdateCommon") void GiantsMask_Update(Player* this, PlayState* play) {
    if (play->sceneId == SCENE_INISIE_BS) {
        return;
    }
    
    HandleCutscene(this, play);
    
    // Scale movement REGs on top of boots values (MMR's method)
    if (sIsGiant && sCsState == GMASK_CS_WAITING) {
        // Only scale if at boots values (not already giant scaled)
        if (REG(68) < 100 && REG(68) > -200) {
            REG(19) *= 10;  // run acceleration
            REG(43) *= 10;  // idle deceleration
            REG(45) *= 10;  // running speed
            REG(48) *= 10;  // slow backwalk threshold
            REG(68) *= 10;  // gravity
            
            // Animation thresholds
            if (REG(32) > 1) REG(32) /= 10;
            if (REG(36) > 1) REG(36) /= 10;
            if (REG(37) > 1) REG(37) /= 10;
            if (REG(38) > 1) REG(38) /= 10;
            
            if (IREG(66) < 1000) IREG(66) *= 10;
            if (IREG(69) > 1) IREG(69) /= 10;
            if (MREG(95) > 1) MREG(95) /= 10;
        }
    }
    
    // Enable button
    if (GET_PLAYER_FORM == this->transformation) {
        for (s16 i = EQUIP_SLOT_C_LEFT; i <= EQUIP_SLOT_C_RIGHT; i++) {
            if (GET_CUR_FORM_BTN_ITEM(i) == ITEM_MASK_GIANT) {
                gSaveContext.buttonStatus[i] = BTN_ENABLED;
            }
        }
    }
}