/**
 * $Id: $
 *
 * @brief Red Pitaya library housekeeping module implementation
 *
 * @Author Red Pitaya
 *
 * (c) Red Pitaya  http://www.redpitaya.com
 *
 * This part of code is written in C programming language.
 * Please visit http://en.wikipedia.org/wiki/C_(programming_language)
 * for more details on the language used herein.
 */


#include <stdio.h>
#include <sys/types.h>

#include "version.h"
#include "common.h"
#include "housekeeping.h"


// Base Housekeeping address
static const int HOUSEKEEPING_BASE_ADDR = 0x40000000;
static const int HOUSEKEEPING_BASE_SIZE = 0x30;

// Housekeeping structure declaration
typedef struct housekeeping_control_s {
	uint32_t id;
	uint32_t dna_part1;
	uint32_t dna_part2;
	uint32_t reserved_1;
	uint32_t ex_cd_p;
	uint32_t ex_cd_n;
	uint32_t ex_co_p;
	uint32_t ex_co_n;
	uint32_t ex_ci_p;
	uint32_t ex_ci_n;
	uint32_t reserved_2;
	uint32_t reserved_3;
	uint32_t led_control;
} housekeeping_control_t;


static const uint32_t LED_CONTROL_MASK = 0xFF;
static const uint32_t EX_CD_P_MASK = 0xFF;
static const uint32_t EX_CD_N_MASK = 0xFF;
static const uint32_t EX_CO_P_MASK = 0xFF;
static const uint32_t EX_CO_N_MASK = 0xFF;
static const uint32_t EX_CI_P_MASK = 0xFF;
static const uint32_t EX_CI_N_MASK = 0xFF;



static volatile housekeeping_control_t *hk = NULL;

/**
* general
*/


int hk_Init()
{
	ECHECK(cmn_Init());
	ECHECK(cmn_Map(HOUSEKEEPING_BASE_SIZE, HOUSEKEEPING_BASE_ADDR, (void**)&hk));
	return RP_OK;
}

int hk_Release()
{
	ECHECK(cmn_Unmap(HOUSEKEEPING_BASE_SIZE, (void**)&hk));
	ECHECK(cmn_Release());
	return RP_OK;
}


/**
* led_control
*/

int hk_SetLedBits(uint32_t bits)
{
	// First led is read only
	if (bits == 0x1) {
		return RP_EMRO;
	}
	return cmn_SetBits(&hk->led_control, bits, LED_CONTROL_MASK);
}

int hk_UnsetLedBits(uint32_t bits)
{
	return cmn_UnsetBits(&hk->led_control, bits, LED_CONTROL_MASK);
}

int hk_AreLedBitsSet(uint32_t bits, bool* result)
{
	return cmn_AreBitsSet(hk->led_control, bits, LED_CONTROL_MASK, result);
}


/**
* ex_cd_p
*/

int hk_SetExCdPBits(uint32_t bits)
{
	return cmn_SetBits(&hk->ex_cd_p, bits, EX_CD_P_MASK);
}

int hk_UnsetExCdPBits(uint32_t bits)
{
	return cmn_UnsetBits(&hk->ex_cd_p, bits, EX_CD_P_MASK);
}

int hk_AreExCdPBitsSet(uint32_t bits, bool* result)
{
	return cmn_AreBitsSet(hk->ex_cd_p, bits, EX_CD_P_MASK, result);
}


/**
* ex_cd_n
*/

int hk_SetExCdNBits(uint32_t bits)
{
	return cmn_SetBits(&hk->ex_cd_n, bits, EX_CD_N_MASK);
}

int hk_UnsetExCdNBits(uint32_t bits)
{
	return cmn_UnsetBits(&hk->ex_cd_n, bits, EX_CD_N_MASK);
}

int hk_AreExCdNBitsSet(uint32_t bits, bool* result)
{
	return cmn_AreBitsSet(hk->ex_cd_n, bits, EX_CD_N_MASK, result);
}


/**
* ex_co_p
*/

int hk_SetExCoPBits(uint32_t bits)
{
	return cmn_SetBits(&hk->ex_co_p, bits, EX_CO_P_MASK);
}

int hk_UnsetExCoPBits(uint32_t bits)
{
	return cmn_UnsetBits(&hk->ex_co_p, bits, EX_CO_P_MASK);
}

int hk_AreExCoPBitsSet(uint32_t bits, bool* result)
{
	return cmn_AreBitsSet(hk->ex_co_p, bits, EX_CO_P_MASK, result);
}



/**
* ex_co_n
*/

int hk_SetExCoNBits(uint32_t bits)
{
	return cmn_SetBits(&hk->ex_co_n, bits, EX_CO_N_MASK);
}

int hk_UnsetExCoNBits(uint32_t bits)
{
	return cmn_UnsetBits(&hk->ex_co_n, bits, EX_CO_N_MASK);
}

int hk_AreExCoNBitsSet(uint32_t bits, bool* result)
{
	return cmn_AreBitsSet(hk->ex_co_n, bits, EX_CO_N_MASK, result);
}



/**
* ex_ci_p
*/

int hk_AreExCiPBitsSet(uint32_t bits, bool* result)
{
	return cmn_AreBitsSet(hk->ex_ci_p, bits, EX_CI_P_MASK, result);
}



/**
* ex_ci_n
*/

int hk_AreExCiNBitsSet(uint32_t bits, bool* result)
{
	return cmn_AreBitsSet(hk->ex_ci_n, bits, EX_CI_N_MASK, result);
}