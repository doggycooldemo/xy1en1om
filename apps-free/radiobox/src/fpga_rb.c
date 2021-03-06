/**
 * @brief Red Pitaya FPGA Interface for the RadioBox sub-module.
 *
 * @author Ulrich Habel (DF4IAH) <espero7757@gmx.net>
 *
 * (c) Red Pitaya  http://www.redpitaya.com
 *
 * This part of code is written in C programming language.
 * Please visit http://en.wikipedia.org/wiki/C_(programming_language)
 * for more details on the language used herein.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include "main.h"
#include "calib.h"
#include "rp_gain_compensation.h"
#include "fpga.h"
#include "cb_http.h"


/** @brief calibration data layout within the EEPROM device */
extern rp_calib_params_t    g_rp_main_calib_params;

/** @brief CallBack copy of params from the worker when requested */
extern rb_app_params_t*     g_rb_info_worker_params;
/** @brief Holds mutex to access on parameters from the worker thread to any other context */
extern pthread_mutex_t      g_rb_info_worker_params_mutex;

/** @brief The RadioBox memory file descriptor used to mmap() the FPGA space. */
extern int                  g_fpga_rb_mem_fd;
/** @brief The RadioBox memory layout of the FPGA registers. */
extern fpga_rb_reg_mem_t*   g_fpga_rb_reg_mem;


/** @brief Filename of the default FPGA configuration. */
const char fn_bit[]       = "/opt/redpitaya/fpga/fpga.bit";

/** @brief Filename of the safed FPGA configuration. */
const char fn_bit_orig[]  = "/opt/redpitaya/fpga/fpga.bit_orig";

/** @brief Filename of a fresh RadioBox FPGA configuration. */
const char fn_bit_fresh[] = "/opt/redpitaya/www/apps/radiobox/fpga.bit";


/*----------------------------------------------------------------------------*/
int fpga_rb_init(void)
{
    //fprintf(stderr, "DEBUG fpga_rb_init: BEGIN\n");

    /* make sure all previous data is vanished */
    fpga_rb_exit();

    /* init the RadioBox FPGA sub-module access */
    if (fpga_mmap_area(&g_fpga_rb_mem_fd, (void**) &g_fpga_rb_reg_mem, FPGA_RB_BASE_ADDR, FPGA_RB_BASE_SIZE)) {
        fprintf(stderr, "ERROR - fpga_rb_init: g_fpga_rb_reg_mem - mmap() failed: %s\n", strerror(errno));
        fpga_exit();
        return -1;
    }
    //fprintf(stderr, "DEBUG fpga_rb_init: g_fpga_rb_reg_mem - having access pointer.\n");

    // Check for valid FPGA
    uint32_t ver = fpga_get_version();
    if ((ver < FPGA_VERSION_MIN) || (ver & 0x80000000)) {  // RadioBox to old or contains no RadioBox sub-module at all
        fprintf(stderr, "INFO - fpga_rb_init: central fpga.bit file outdated - FPGA found: %08x, requested: %08x\n", ver, FPGA_VERSION_MIN);

        // do a fresh set-up
        fpga_rb_exit();

        // move current fpga.bit file out of the way and copy local file to the central directory
        fpga_rb_prepare_file();

        // reload new configuration to the FPGA
        fpga_rb_reload_fpga();

        // try again to map the new FPGA configuration
        if (fpga_mmap_area(&g_fpga_rb_mem_fd, (void**) &g_fpga_rb_reg_mem, FPGA_RB_BASE_ADDR, FPGA_RB_BASE_SIZE)) {
            fprintf(stderr, "ERROR - fpga_rb_init: g_fpga_rb_reg_mem - mmap() failed: %s\n", strerror(errno));
            fpga_exit();
            return -1;
        }
    }

    // enable RadioBox sub-module
    fpga_rb_reset();
    fpga_rb_enable(1);

    //fprintf(stderr, "DEBUG fpga_rb_init: END\n");
    return 0;
}

/*----------------------------------------------------------------------------*/
int fpga_rb_exit(void)
{
    //fprintf(stderr, "fpga_rb_exit: BEGIN\n");

    /* disable RadioBox sub-module */
    fpga_rb_enable(0);

    /* unmap the RadioBox sub-module */
    if (fpga_munmap_area(&g_fpga_rb_mem_fd, (void**) &g_fpga_rb_reg_mem, FPGA_RB_BASE_ADDR, FPGA_RB_BASE_SIZE)) {
        fprintf(stderr, "ERROR - fpga_rb_exit: g_fpga_rb_reg_mem - munmap() failed: %s\n", strerror(errno));
    }

    //fprintf(stderr, "fpga_rb_exit: END\n");
    return 0;
}

/*----------------------------------------------------------------------------*/
void fpga_rb_enable(int enable)
{
    if (!g_fpga_rb_reg_mem) {
        return;
    }

    //fprintf(stderr, "DEBUG - fpga_rb_enable(%d): BEGIN\n", enable);

    if (enable) {
        // enable RadioBox
        g_fpga_rb_reg_mem->ctrl           = 0x00000001;    // enable RB sub-module

        g_fpga_rb_reg_mem->src_con_pnt    = 0x301C0000;    // disable RB LEDs, set RFOUT1 to AMP_RF output and RFOUT2 to RX_MOD_ADD output
        g_fpga_rb_reg_mem->tx_muxin_gain  = 0x00007FFF;    // open Mic gain 1:1 (FS = 2Vpp) = 80 % Mic gain setting

        g_fpga_rb_reg_mem->tx_amp_rf_gain = 0x00000C80;    // open RF output at -10 dBm (= 200 mVpp @ 50 Ohm)
        g_fpga_rb_reg_mem->tx_amp_rf_ofs  = 0;             // no corrections done

    } else {
        //fprintf(stderr, "fpga_rb_enable: turning off RB LEDs\n");
        g_fpga_rb_reg_mem->src_con_pnt    = 0x00000000;    // disable RB LEDs, RFOUT1 and RFOUT2
        g_fpga_rb_reg_mem->tx_muxin_gain  = 0x00000000;    // shut Mic input

        g_fpga_rb_reg_mem->tx_amp_rf_gain = 0;             // no output

        g_fpga_rb_reg_mem->rx_muxin_src   = 0x00000000;    // disable receiver input MUX

        // disable RadioBox
        //fprintf(stderr, "fpga_rb_enable: disabling RB sub-module\n");
        g_fpga_rb_reg_mem->ctrl           = 0x00000000;    // disable RB sub-module
    }

    //fprintf(stderr, "DEBUG - fpga_rb_enable(%d): END\n", enable);
}

/*----------------------------------------------------------------------------*/
void fpga_rb_reset(void)
{
    if (!g_fpga_rb_reg_mem) {
        return;
    }

    //fprintf(stderr, "INFO - fpga_rb_reset\n");

    /* reset all registers of the TX_MOD_OSC to get fixed phase of 0 deg */
    g_fpga_rb_reg_mem->tx_mod_osc_inc_lo = 0;
    g_fpga_rb_reg_mem->tx_mod_osc_inc_hi = 0;
    g_fpga_rb_reg_mem->tx_mod_osc_ofs_lo = 0;
    g_fpga_rb_reg_mem->tx_mod_osc_ofs_hi = 0;

    /* reset all registers of the TX_CAR_OSC to get fixed phase of 0 deg */
    g_fpga_rb_reg_mem->tx_car_osc_inc_lo = 0;
    g_fpga_rb_reg_mem->tx_car_osc_inc_hi = 0;
    g_fpga_rb_reg_mem->tx_car_osc_ofs_lo = 0;
    g_fpga_rb_reg_mem->tx_car_osc_ofs_hi = 0;

    /* reset all registers of the RX_MOD_OSC to get fixed phase of 0 deg */
    g_fpga_rb_reg_mem->rx_mod_osc_inc_lo = 0;
    g_fpga_rb_reg_mem->rx_mod_osc_inc_hi = 0;
    g_fpga_rb_reg_mem->rx_mod_osc_ofs_lo = 0;
    g_fpga_rb_reg_mem->rx_mod_osc_ofs_hi = 0;

    /* reset all registers of the RX_CAR_OSC to get fixed phase of 0 deg */
    g_fpga_rb_reg_mem->rx_car_osc_inc_lo = 0;
    g_fpga_rb_reg_mem->rx_car_osc_inc_hi = 0;
    g_fpga_rb_reg_mem->rx_car_osc_ofs_lo = 0;
    g_fpga_rb_reg_mem->rx_car_osc_ofs_hi = 0;

    /* send resync to all oscillators to zero phase registers, all streams are turned off */
    g_fpga_rb_reg_mem->ctrl = 0x10101011;

    /* send resync and reset to all oscillators */
    g_fpga_rb_reg_mem->ctrl = 0x10161017;

    /* send resync to all oscillators to zero phase registers */
    g_fpga_rb_reg_mem->ctrl = 0x10101011;

    /* run mode of both oscillators */
    g_fpga_rb_reg_mem->ctrl = 0x00000001;
}

/*----------------------------------------------------------------------------*/
uint32_t fpga_get_version()
{
    if (!g_fpga_rb_reg_mem) {
        return -1;
    }

    uint32_t version = g_fpga_rb_reg_mem->version;
    fprintf(stderr, "INFO - fpga_get_version: current FPGA RadioBox version: %08x\n", version);

    if (version < 0x12010101 || version > 0x29123299) {
        //fprintf(stderr, "DEBUG - fpga_get_version: error -2\n");
        return -2;
    }

    int pos;
    for (pos = 28; pos >= 0; pos -= 4) {
        if (((version >> pos) & 0xf) > 0x9) {  // no HEX entries allowed as date and serial number
            //fprintf(stderr, "DEBUG - fpga_get_version: error -3\n");
            return -3;
        }
    }

    return version;  // valid date found
}

/*----------------------------------------------------------------------------*/
int fpga_rb_prepare_file()
{
    struct stat sb         = { 0 };
    const int CMDBUFLEN    = 256;
    char cmdbuf[CMDBUFLEN];

    //fprintf(stderr, "DEBUG - fpga_rb_prepare_file: begin\n");

    // make the partition RW accessible
    system("/opt/redpitaya/sbin/rw");

    // safe the original fpga.bit file if not already done
    if (stat(fn_bit_orig, &sb)) {
        // safe the non-safed bit-file
        rename(fn_bit, fn_bit_orig);
    } else {
        // do not safe, but remove old RadioBox FPGA file
        remove(fn_bit);
    }

    // copy local fresh RadioBox FPGA configuration to the centralized place
    snprintf(cmdbuf, CMDBUFLEN - 1, "cp -a %s %s", fn_bit_fresh, fn_bit);
    system(cmdbuf);

    // make the partition RO again
    system("/opt/redpitaya/sbin/ro");

    //fprintf(stderr, "DEBUG - fpga_rb_prepare_file: end\n");
    return 0;
}

/*----------------------------------------------------------------------------*/
void fpga_rb_reload_fpga()
{
    const int CMDBUFLEN = 256;
    char cmdbuf[CMDBUFLEN];

    //fprintf(stderr, "DEBUG - fpga_rb_reload_fpga: begin\n");

    snprintf(cmdbuf, CMDBUFLEN - 1, "cat %s >/dev/xdevcfg", fn_bit);
    system(cmdbuf);

    //fprintf(stderr, "DEBUG - fpga_rb_reload_fpga: end\n");
}

/*----------------------------------------------------------------------------*/
void fpga_rb_calib(int calib, int enabled)
{
    if (calib > 0) {
        rp_measure_calib_params(&g_rp_main_calib_params);
    }

    fpga_rb_enable(enabled);
}

/*----------------------------------------------------------------------------*/
int fpga_rb_update_all_params(rb_app_params_t* pb, rb_app_params_t** p_pn)  // pb: base data of complete data set, pn: new overwriting data sets
{
    int    loc_rb_run         = 0;
    int    loc_rb_calib       = 0;
    int    loc_tx_modsrc      = 0;
    int    loc_tx_modtyp      = 0;
    int    loc_rx_modtyp      = 0;
    int    loc_led_csp        = 0;
    int    loc_rfout1_csp     = 0;
    int    loc_rfout2_csp     = 0;
    int    loc_rx_muxin_src   = 0;
    double loc_tx_car_osc_qrg = 0.0;
    double loc_rx_car_osc_qrg = 0.0;
    double loc_tx_mod_osc_qrg = 0.0;
    int    loc_tx_amp_rf_gain = 0;
    int    loc_tx_mod_osc_mag = 0;
    int    loc_tx_muxin_gain  = 0;
    int    loc_rx_muxin_gain  = 0;
    int    loc_tx_qrg_sel     = 0;
    int    loc_rx_qrg_sel     = 0;
    int    loc_rfout1_term    = 0;
    int    loc_rfout2_term    = 0;
    int    loc_qrg_inc        = 0;
    int    loc_ac97_lil       = 0;
    int    loc_ac97_lir       = 0;

    //fprintf(stderr, "DEBUG - fpga_rb_update_all_params: BEGIN\n");

    if (!g_fpga_rb_reg_mem || !pb || !p_pn) {
        fprintf(stderr, "ERROR - fpga_rb_update_all_params: bad parameter (pb=%p), (p_pn=%p) or not init'ed(g=%p)\n", pb, p_pn, g_fpga_rb_reg_mem);
        return -1;
    }
    rb_app_params_t* pn = *p_pn;
    if (!pn) {
        fprintf(stderr, "ERROR - fpga_rb_update_all_params: bad parameter (pn=%p)\n", pn);
        return -2;
    }

    /* Get current parameters from the worker */
    {
        //fprintf(stderr, "DEBUG - fpga_rb_update_all_params: getting local data: ...\n");
        //print_rb_params(pb);
        loc_rb_run         = (int) pb[RB_RUN].value;
        loc_rb_calib       = 0;
        loc_tx_modsrc      = (int) pb[RB_TX_MODSRC].value;
        loc_tx_modtyp      = (int) pb[RB_TX_MODTYP].value;
        loc_rx_modtyp      = (int) pb[RB_RX_MODTYP].value;
        loc_led_csp        = (int) pb[RB_LED_CON_SRC_PNT].value;
        loc_rfout1_csp     = (int) pb[RB_RFOUT1_CON_SRC_PNT].value;
        loc_rfout2_csp     = (int) pb[RB_RFOUT2_CON_SRC_PNT].value;
        loc_rx_muxin_src   = (int) pb[RB_RX_MUXIN_SRC].value;

        loc_tx_car_osc_qrg = pb[RB_TX_CAR_OSC_QRG].value;
        loc_rx_car_osc_qrg = pb[RB_RX_CAR_OSC_QRG].value;

        loc_tx_mod_osc_qrg = pb[RB_TX_MOD_OSC_QRG].value;
        loc_tx_muxin_gain  = (int) pb[RB_TX_MUXIN_GAIN].value;
        loc_rx_muxin_gain  = (int) pb[RB_RX_MUXIN_GAIN].value;
        loc_tx_qrg_sel     = (int) pb[RB_TX_QRG_SEL].value;
        loc_rx_qrg_sel     = (int) pb[RB_RX_QRG_SEL].value;

        loc_tx_amp_rf_gain = (int) pb[RB_TX_AMP_RF_GAIN].value;
        loc_tx_mod_osc_mag = (int) pb[RB_TX_MOD_OSC_MAG].value;
        loc_rfout1_term    = (int) pb[RB_RFOUT1_TERM].value;
        loc_rfout2_term    = (int) pb[RB_RFOUT2_TERM].value;
        loc_qrg_inc        = (int) pb[RB_QRG_INC].value;
        loc_ac97_lil       = (int) pb[RB_AC97_LOL].value;
        loc_ac97_lir       = (int) pb[RB_AC97_LOR].value;
        //fprintf(stderr, "DEBUG - fpga_rb_update_all_params: ... done.\n");
    }

    int idx;
    for (idx = 0; pn[idx].name; idx++) {
        if (!(pn[idx].name)) {
            break;  // end of list
        }

        if (!(pn[idx].fpga_update & 0x80)) {  // MARKer set?
            //fprintf(stderr, "DEBUG - fpga_rb_update_all_params: skipped not modified parameter (name=%s)\n", pn[idx].name);
            continue;  // this value is not marked to update the FPGA
        }
        //fprintf(stderr, "DEBUG - fpga_rb_update_all_params: this parameter has to update the FPGA (name=%s)\n", p[idx].name);

        /* Remove the marker */
        pn[idx].fpga_update &= ~0x80;

        /* Since here process on each known parameter accordingly */
        if (!strcmp("rb_run", pn[idx].name)) {
            //fprintf(stderr, "INFO - fpga_rb_update_all_params: #got rb_run = %d\n", (int) (pn[idx].value));
            loc_rb_run = ((int) (pn[idx].value));
            fpga_rb_enable(loc_rb_run);

        } else if (!strcmp("rb_calib", pn[idx].name)) {
            //fprintf(stderr, "INFO - fpga_rb_update_all_params: #got rb_calib = %d\n", (int) (pn[idx].value));
            loc_rb_calib = ((int) (pn[idx].value));
            fpga_rb_calib(loc_rb_calib, loc_rb_run);
            pn[idx].value = 0.0;  // remove single-shot tag

        } else if (!strcmp("tx_modsrc_s", pn[idx].name)) {
            //fprintf(stderr, "INFO - fpga_rb_update_all_params: #got tx_modsrc_s = %d\n", (int) (pn[idx].value));
            loc_tx_modsrc = ((int) (pn[idx].value));

        } else if (!strcmp("tx_modtyp_s", pn[idx].name)) {
            //fprintf(stderr, "INFO - fpga_rb_update_all_params: #got tx_modtyp_s = %d\n", (int) (pn[idx].value));
            loc_tx_modtyp = ((int) (pn[idx].value));

        } else if (!strcmp("rx_modtyp_s", pn[idx].name)) {
            //fprintf(stderr, "INFO - fpga_rb_update_all_params: #got rx_modtyp_s = %d\n", (int) (pn[idx].value));
            loc_rx_modtyp = ((int) (pn[idx].value));

        } else if (!strcmp("rbled_csp_s", pn[idx].name)) {
            //fprintf(stderr, "INFO - fpga_rb_update_all_params: #got rbled_csp_s = %d\n", (int) (pn[idx].value));
            loc_led_csp = ((int) (pn[idx].value));

        } else if (!strcmp("rfout1_csp_s", pn[idx].name)) {
            //fprintf(stderr, "INFO - fpga_rb_update_all_params: #got rfout1_csp_s = %d\n", (int) (pn[idx].value));
            loc_rfout1_csp = ((int) (pn[idx].value));

        } else if (!strcmp("rfout2_csp_s", pn[idx].name)) {
            //fprintf(stderr, "INFO - fpga_rb_update_all_params: #got rfout2_csp_s = %d\n", (int) (pn[idx].value));
            loc_rfout2_csp = ((int) (pn[idx].value));

        } else if (!strcmp("rx_muxin_src_s", pn[idx].name)) {
            //fprintf(stderr, "INFO - fpga_rb_update_all_params: #got rx_muxin_src_s = %d\n", (int) (pn[idx].value));
            loc_rx_muxin_src = ((int) (pn[idx].value));


        } else if (!strcmp("tx_car_osc_qrg_f", pn[idx].name)) {
            //fprintf(stderr, "INFO - fpga_rb_update_all_params: #got tx_car_osc_qrg_f = %lf\n", pn[idx].value);
            loc_tx_car_osc_qrg = pn[idx].value;

        } else if (!strcmp("rx_car_osc_qrg_f", pn[idx].name)) {
            //fprintf(stderr, "INFO - fpga_rb_update_all_params: #got rx_car_osc_qrg_f = %lf\n", pn[idx].value);
            loc_rx_car_osc_qrg = pn[idx].value;


        } else if (!strcmp("tx_mod_osc_qrg_f", pn[idx].name)) {
            //fprintf(stderr, "INFO - fpga_rb_update_all_params: #got tx_mod_osc_qrg_f = %lf\n", pn[idx].value);
            loc_tx_mod_osc_qrg = pn[idx].value;

        } else if (!strcmp("tx_amp_rf_gain_s", pn[idx].name)) {
            //fprintf(stderr, "INFO - fpga_rb_update_all_params: #got tx_amp_rf_gain_f = %lf\n", pn[idx].value);
            loc_tx_amp_rf_gain = ((int) pn[idx].value);

        } else if (!strcmp("tx_mod_osc_mag_s", pn[idx].name)) {
            //fprintf(stderr, "INFO - fpga_rb_update_all_params: #got tx_mod_osc_mag_f = %lf\n", pn[idx].value);
            loc_tx_mod_osc_mag = ((int) pn[idx].value);

        } else if (!strcmp("tx_muxin_gain_s", pn[idx].name)) {
            //fprintf(stderr, "INFO - fpga_rb_update_all_params: #got tx_muxin_gain_s = %d\n", pn[idx].value);
            loc_tx_muxin_gain = ((int) pn[idx].value);

        } else if (!strcmp("rx_muxin_gain_s", pn[idx].name)) {
            //fprintf(stderr, "INFO - fpga_rb_update_all_params: #got rx_muxin_gain_s = %d\n", pn[idx].value);
            loc_rx_muxin_gain = ((int) pn[idx].value);


        } else if (!strcmp("tx_qrg_sel_s", pn[idx].name)) {
            //fprintf(stderr, "INFO - fpga_rb_update_all_params: #got tx_qrg_sel_s = %d\n", (int) (pn[idx].value));
            loc_tx_qrg_sel = ((int) (pn[idx].value));

        } else if (!strcmp("rx_qrg_sel_s", pn[idx].name)) {
            //fprintf(stderr, "INFO - fpga_rb_update_all_params: #got rx_qrg_sel_s = %d\n", (int) (pn[idx].value));
            loc_rx_qrg_sel = ((int) (pn[idx].value));

        } else if (!strcmp("rfout1_term_s", pn[idx].name)) {
            //fprintf(stderr, "INFO - fpga_rb_update_all_params: #got rfout1_term_s = %d\n", (int) (pn[idx].value));
            loc_rfout1_term = ((int) (pn[idx].value));

        } else if (!strcmp("rfout2_term_s", pn[idx].name)) {
            //fprintf(stderr, "INFO - fpga_rb_update_all_params: #got rfout2_term_s = %d\n", (int) (pn[idx].value));
            loc_rfout2_term = ((int) (pn[idx].value));

        } else if (!strcmp("qrg_inc_s", pn[idx].name)) {
            //fprintf(stderr, "INFO - fpga_rb_update_all_params: #got qrg_inc_s = %d\n", (int) (pn[idx].value));
            loc_qrg_inc = ((int) (pn[idx].value));

        } else if (!strcmp("ac97_lil_s", pn[idx].name)) {
            //fprintf(stderr, "INFO - fpga_rb_update_all_params: #got ac97_lil_s = %d\n", (int) (pn[idx].value));
            loc_ac97_lil = ((int) (pn[idx].value));

        } else if (!strcmp("ac97_lir_s", pn[idx].name)) {
            //fprintf(stderr, "INFO - fpga_rb_update_all_params: #got ac97_lir_s = %d\n", (int) (pn[idx].value));
            loc_ac97_lir = ((int) (pn[idx].value));

        }  // else if ()
    }  // for ()

    /* set the new values */
    {
        if (loc_rb_run) {
            fpga_rb_set_ctrl(loc_rb_run, loc_tx_modsrc, loc_tx_modtyp, loc_rx_modtyp,
                    ((loc_rfout2_csp  & 0xff) << 0x18) | ((loc_rfout1_csp  & 0xff) << 0x10) | (loc_led_csp & 0xff),
                    ((loc_ac97_lir    & 0xff) << 0x08) | ((loc_ac97_lil    & 0xff) << 0x00),
                    loc_rx_muxin_src,
                    loc_tx_car_osc_qrg, loc_rx_car_osc_qrg,
                    loc_tx_mod_osc_qrg, loc_tx_muxin_gain, loc_rx_muxin_gain, loc_tx_qrg_sel, loc_rx_qrg_sel,
                    loc_tx_amp_rf_gain, loc_tx_mod_osc_mag, loc_rfout1_term, loc_rfout2_term, loc_qrg_inc);
        }
    }

    //fprintf(stderr, "DEBUG - fpga_rb_update_all_params: END\n");
    return 0;
}

/*----------------------------------------------------------------------------*/
int fpga_rb_get_fpga_params(rb_app_params_t* pb, rb_app_params_t** p_pn)  // pb: base data of complete data set, pn: new overwriting data sets
{
  int    loc_tx_modtyp      = 0;
  int    loc_rx_modtyp      = 0;
  int    loc_qrg_inc        = 0;

  //fprintf(stderr, "DEBUG - fpga_rb_get_fpga_params: BEGIN\n");

  if (!g_fpga_rb_reg_mem || !pb || !p_pn) {
    fprintf(stderr, "ERROR - fpga_rb_get_fpga_params: bad parameter (pb=%p), (p_pn=%p) or not init'ed(g=%p)\n", pb, p_pn, g_fpga_rb_reg_mem);
    return -1;
  }
  rb_app_params_t* pn = *p_pn;
  if (!pn) {
    fprintf(stderr, "ERROR - fpga_rb_get_fpga_params: bad parameter (pn=%p)\n", pn);
    return -2;
  }

  /* Get base parameters from the worker */
  {
      //fprintf(stderr, "DEBUG - fpga_rb_get_fpga_params: getting local data: ...\n");
      //print_rb_params(pb);
      loc_tx_modtyp      = (int) pb[RB_TX_MODTYP].value;
      loc_rx_modtyp      = (int) pb[RB_RX_MODTYP].value;
      loc_qrg_inc        = (int) pb[RB_QRG_INC].value;
      //fprintf(stderr, "DEBUG - fpga_rb_get_fpga_params: ... done.\n");
  }

  /* Get current parameters from the worker */
  {
    int idx;
    for (idx = 0; pn[idx].name; idx++) {
      if (!(pn[idx].name)) {
        break;  // end of list

      } else if (!strcmp("tx_modtyp_s", pn[idx].name)) {
        //fprintf(stderr, "INFO - fpga_rb_get_fpga_params: #got tx_modtyp_s = %d, was = %d\n", (int) (pn[idx].value), loc_tx_modtyp);
        loc_tx_modtyp = ((int) (pn[idx].value));

      } else if (!strcmp("rx_modtyp_s", pn[idx].name)) {
        //fprintf(stderr, "INFO - fpga_rb_get_fpga_params: #got rx_modtyp_s = %d, was = %d\n", (int) (pn[idx].value), loc_rx_modtyp);
        loc_rx_modtyp = ((int) (pn[idx].value));

      } else if (!strcmp("qrg_inc_s", pn[idx].name)) {
        //fprintf(stderr, "INFO - fpga_rb_get_fpga_params: #got qrg_inc_s = %d, was = %d\n", (int) (pn[idx].value), loc_qrg_inc);
        loc_qrg_inc = ((int) (pn[idx].value));
      }  // else if ()
    }  // for ()
  }

  /* get current FPGA settings */
  {
    double   loc_RD_tx_car_osc_qrg = 0.0;
    double   loc_RD_rx_car_osc_qrg = 0.0;
    uint16_t loc_RD_ovrdrv         = 0;

    //fprintf(stderr, "DEBUG - fpga_rb_update_all_params - get current FPGA settings ...\n");
    fpga_rb_get_ctrl(
            loc_tx_modtyp,
            loc_rx_modtyp,
            &loc_RD_tx_car_osc_qrg,
            &loc_RD_rx_car_osc_qrg,
            &loc_RD_ovrdrv);

    if (loc_qrg_inc != 50) {
      rb_update_param(p_pn, "tx_car_osc_qrg_f", loc_RD_tx_car_osc_qrg);
      rb_update_param(p_pn, "rx_car_osc_qrg_f", loc_RD_rx_car_osc_qrg);
    }

    rb_update_param(p_pn, "ovrdrv_s", loc_RD_ovrdrv);

    //print_rb_params(*p_pn);
  }

  //fprintf(stderr, "DEBUG - fpga_rb_get_fpga_params: END\n");
  return 1;
}


/*----------------------------------------------------------------------------*/
void fpga_rb_set_ctrl(int rb_run, int tx_modsrc, int tx_modtyp, int rx_modtyp, int src_con_pnt, int src_con_pnt2, int rx_muxin_src,
        double tx_car_osc_qrg, double rx_car_osc_qrg,
        double tx_mod_osc_qrg, int tx_muxin_gain, int rx_muxin_gain, int tx_qrg_sel, int rx_qrg_sel,
        int tx_amp_rf_gain, int tx_mod_osc_mag, int term_rfout1, int term_rfout2, int qrg_inc)
{
    const  int      ssb_weaver_osc_qrg = 1700.0;
    static double   tx_car_osc_qrg_old = 0.0;
    static double   rx_car_osc_qrg_old = 0.0;
    static uint32_t src_con_pnt_old = 0;
    static uint32_t src_con_pnt2_old = 0;
    static int      term_rfout1_old = 0;
    static int      term_rfout2_old = 0;
    double          rfout1_amp_gain = 0.0;
    double          rfout2_amp_gain = 0.0;
    int             tx_car_osc_qrg_inc = 50;
    int             rx_car_osc_qrg_inc = 50;
    uint32_t        adc_auto_ofs = 0x00000000;

#if 1
    adc_auto_ofs = 0x01000000;                                                                             // enable ADC automatic offset compensation
#endif

    //fprintf(stderr, "DEBUG - fpga_rb_set_ctrl: checking tx_qrg_sel = %d, rx_qrg_sel = %d, qrg_inc = %d\n", tx_qrg_sel, rx_qrg_sel, qrg_inc);
    if (tx_qrg_sel) {
      //fprintf(stderr, "DEBUG - fpga_rb_set_ctrl: setting tx_car_osc_qrg_inc = %+3d\n", qrg_inc);
      tx_car_osc_qrg_inc = qrg_inc;
    }
    if (rx_qrg_sel) {
      //fprintf(stderr, "DEBUG - fpga_rb_set_ctrl: setting rx_car_osc_qrg_inc = %+3d\n", qrg_inc);
      rx_car_osc_qrg_inc = qrg_inc;
    }

    //fprintf(stderr, "INFO - fpga_rb_set_ctrl: rb_run=%d, tx_modsrc=%d, tx_modtyp=%d, src_con_pnt=%d, term=%d, tx_car_osc_qrg=%lf, tx_mod_osc_qrg=%lf, tx_amp_rf_gain=%d, tx_mod_osc_mag=%d, tx_muxin_gain=%d\n",
    //        rb_run, tx_modsrc, tx_modtyp, src_con_pnt, term, tx_car_osc_qrg, tx_mod_osc_qrg, tx_amp_rf_gain, tx_mod_osc_mag, tx_muxin_gain);

    /* RF Out x Gain settings - only when relevant changes were made */
    if (
        (src_con_pnt_old    != src_con_pnt)    ||
        (src_con_pnt2_old   != src_con_pnt2)   ||
        (term_rfout1_old    != term_rfout1)    ||
        (term_rfout2_old    != term_rfout2)    ||
        (tx_car_osc_qrg_old != tx_car_osc_qrg) ||
        (rx_car_osc_qrg_old != rx_car_osc_qrg)) {
      //fprintf(stderr, "DEBUG - fpga_rb_set_ctrl: setting src_con_pnt to new value = 0x%08x, old = 0x%08x / term = %d, term_old = %d\n", src_con_pnt, src_con_pnt_old, term, term_old);
      tx_car_osc_qrg_old = tx_car_osc_qrg;
      rx_car_osc_qrg_old = rx_car_osc_qrg;
      src_con_pnt_old = src_con_pnt;
      term_rfout1_old = term_rfout1;
      term_rfout2_old = term_rfout2;

      double rfout_frequency[2];
      uint8_t rfout_con_pnt[2];
      rfout_con_pnt[0] = (src_con_pnt & 0x00ff0000) >> 16;
      rfout_con_pnt[1] = (src_con_pnt & 0xff000000) >> 24;

      int rfout_i;
      for (rfout_i = 0; rfout_i <= 1; rfout_i++) {
        if ((0x04 <= rfout_con_pnt[rfout_i]) && (rfout_con_pnt[rfout_i] <= 0x17)) {
          rfout_frequency[rfout_i] = ssb_weaver_osc_qrg;  // represents audio frequencies

        } else if ((0x18 <= rfout_con_pnt[rfout_i]) && (rfout_con_pnt[rfout_i] <= 0x1f)) {
          rfout_frequency[rfout_i] = tx_car_osc_qrg;

        } else if ((0x20 <= rfout_con_pnt[rfout_i]) && (rfout_con_pnt[rfout_i] <= 0x21)) {
          rfout_frequency[rfout_i] = rx_car_osc_qrg;

        } else if ((0x22 <= rfout_con_pnt[rfout_i]) && (rfout_con_pnt[rfout_i] <= 0x41)) {
          rfout_frequency[rfout_i] = ssb_weaver_osc_qrg;  // represents audio frequencies

        } else if ((0x48 <= rfout_con_pnt[rfout_i]) && (rfout_con_pnt[rfout_i] <= 0x50)) {
          rfout_frequency[rfout_i] = ssb_weaver_osc_qrg;  // represents audio frequencies

        } else {
          rfout_frequency[rfout_i] = 0.0;  // default value for switch off gain correction
        }
      }

      if (term_rfout1) {
        rfout1_amp_gain = get_compensation_factor(rfout_frequency[0], (term_rfout1 == 0x01 ?  1 : 0));
      } else {
        rfout1_amp_gain = 1.0;
      }
      if (term_rfout2) {
        rfout2_amp_gain = get_compensation_factor(rfout_frequency[1], (term_rfout2 == 0x01 ?  1 : 0));
      } else {
        rfout2_amp_gain = 1.0;
      }
      //fprintf(stderr, "DEBUG - fpga_rb_set_ctrl: get_compensation_factor(rfout1_term_on = %d, QRG = %f) = %f, get_compensation_factor(rfout2_term_on = %d, QRG = %f) = %f\n",
      //        term_rfout1, rfout_frequency[0], rfout1_amp_gain,
      //        term_rfout2, rfout_frequency[1], rfout2_amp_gain);

      fpga_rb_set_rfout1_gain_ofs(rfout1_amp_gain, 0.0);                                                   // RFOUT1_AMP    gain correction setting of the RF Output 1 line, DAC offset value
      fpga_rb_set_rfout2_gain_ofs(rfout2_amp_gain, 0.0);                                                   // RFOUT2_AMP    gain correction setting of the RF Output 2 line, DAC offset value

      g_fpga_rb_reg_mem->src_con_pnt  = src_con_pnt;
      g_fpga_rb_reg_mem->src_con_pnt2 = src_con_pnt2;
    }

    if (rb_run) {
      fpga_rb_set_rx_mod_ssb_am_gain__4mod_ssb_am(100.0);                                                  // RX_MOD_SSB_AM gain setting [ %] only for the SSB demodulator
      fpga_rb_set_rx_mod_amenv_gain__4mod_amenv(100.0);                                                    // RX_MOD_AMENV  gain setting [ %] only for the AM-Envelope demodulator
      fpga_rb_set_rx_mod_fm_gain__4mod_fm(100.0);                                                          // RX_MOD_FM     gain setting [ %] only for the FM demodulator
      fpga_rb_set_rx_mod_pm_gain__4mod_pm(100.0);                                                          // RX_MOD_PM     gain setting [ %] only for the PM demodulator

      switch (tx_modsrc) {

      default:
      case RB_MODSRC_NONE: {
        //fprintf(stderr, "INFO - fpga_rb_set_ctrl: setting FPGA tx_modsrc to (none)\n");

        fpga_rb_set_tx_muxin_gain(0, 0x0000);                                                              // TX MUXIN gain setting
        g_fpga_rb_reg_mem->tx_muxin_src = 0x00000000;
        if (tx_car_osc_qrg_inc == 50) {
          fpga_rb_set_tx_car_osc_qrg__4mod_cw_ssb_am_pm(tx_car_osc_qrg);                                   // TX_CAR_OSC frequency
        }
        fpga_rb_set_tx_car_osc_qrg_inc__4mod_cw_ssb_am_pm(tx_car_osc_qrg_inc);                             // TX_CAR_OSC frequency sweep increment
        fpga_rb_set_tx_mod_qmix_gain_ofs__4mod_cw_ssbweaver_am(0.0, 1);                                    // CW operation
        if (!(g_fpga_rb_reg_mem->status & 0x00000100)) {
          // TX_MOD_OSC phase not zero: reset phase oscillator
          g_fpga_rb_reg_mem->ctrl &= ~0x00001000;                                                          // TX_MOD RESYNC activate
          g_fpga_rb_reg_mem->ctrl |=  0x00001000;                                                          // TX_MOD RESYNC deactivate
          g_fpga_rb_reg_mem->ctrl |= adc_auto_ofs;                                                         // ADC automatic offset compensation
        }
      }
      break;

      case RB_MODSRC_MOD_OSC: {
        //fprintf(stderr, "INFO - fpga_rb_set_ctrl: setting FPGA tx_modsrc to TX_MOD_OSC\n");

        fpga_rb_set_tx_muxin_gain(0, 0x0000);                                                              // TX MUXIN gain setting
        g_fpga_rb_reg_mem->tx_muxin_src = 0x00000000;
      }
      break;

      case RB_MODSRC_RF_IN1: {
        //fprintf(stderr, "INFO - fpga_rb_set_ctrl: setting FPGA tx_modsrc to RF_inp_1\n");

        fpga_rb_set_tx_muxin_gain(tx_muxin_gain, calib_get_ADC_offset(&g_rp_main_calib_params, 0x20));     // TX MUXIN gain setting
        g_fpga_rb_reg_mem->tx_muxin_src = 0x00000020;                                                      // source ID: 32
        g_fpga_rb_reg_mem->ctrl |= adc_auto_ofs;                                                           // ADC automatic offset compensation
      }
      break;

      case RB_MODSRC_RF_IN2: {
        //fprintf(stderr, "INFO - fpga_rb_set_ctrl: setting FPGA tx_modsrc to RF_inp_2\n");

        fpga_rb_set_tx_muxin_gain(tx_muxin_gain, calib_get_ADC_offset(&g_rp_main_calib_params, 0x21));     // TX MUXIN gain setting
        g_fpga_rb_reg_mem->tx_muxin_src = 0x00000021;                                                      // source ID: 33
        g_fpga_rb_reg_mem->ctrl |= adc_auto_ofs;                                                           // ADC automatic offset compensation
      }
      break;

      case RB_MODSRC_EXP_AI0: {
        //fprintf(stderr, "INFO - fpga_rb_set_ctrl: setting FPGA tx_modsrc to AI0\n");

        fpga_rb_set_tx_muxin_gain(tx_muxin_gain, calib_get_ADC_offset(&g_rp_main_calib_params, 0x10));     // TX MUXIN gain setting
        g_fpga_rb_reg_mem->tx_muxin_src = 0x00000010;                                                      // source ID: 16
        g_fpga_rb_reg_mem->ctrl |= adc_auto_ofs;                                                           // ADC automatic offset compensation
      }
      break;

      case RB_MODSRC_EXP_AI1: {
        //fprintf(stderr, "INFO - fpga_rb_set_ctrl: setting FPGA tx_modsrc to AI1\n");

        fpga_rb_set_tx_muxin_gain(tx_muxin_gain, calib_get_ADC_offset(&g_rp_main_calib_params, 0x18));     // TX MUXIN gain setting
        g_fpga_rb_reg_mem->tx_muxin_src = 0x00000018;                                                      // source ID: 24
        g_fpga_rb_reg_mem->ctrl |= adc_auto_ofs;                                                           // ADC automatic offset compensation
      }
      break;

      case RB_MODSRC_EXP_AI2: {
        //fprintf(stderr, "INFO - fpga_rb_set_ctrl: setting FPGA tx_modsrc to AI2\n");

        fpga_rb_set_tx_muxin_gain(tx_muxin_gain, calib_get_ADC_offset(&g_rp_main_calib_params, 0x11));     // TX MUXIN gain setting
        g_fpga_rb_reg_mem->tx_muxin_src = 0x00000011;                                                      // source ID: 17
        g_fpga_rb_reg_mem->ctrl |= adc_auto_ofs;                                                           // ADC automatic offset compensation
      }
      break;

      case RB_MODSRC_EXP_AI3: {
        //fprintf(stderr, "INFO - fpga_rb_set_ctrl: setting FPGA tx_modsrc to AI3\n");

        fpga_rb_set_tx_muxin_gain(tx_muxin_gain, calib_get_ADC_offset(&g_rp_main_calib_params, 0x19));     // TX MUXIN gain setting
        g_fpga_rb_reg_mem->tx_muxin_src = 0x00000019;                                                      // source ID: 25
        g_fpga_rb_reg_mem->ctrl |= adc_auto_ofs;                                                           // ADC automatic offset compensation
      }
      break;

#if 0
      case RB_MODSRC_VP_VN: {
        fpga_rb_set_tx_muxin_gain(tx_muxin_gain, calib_get_ADC_offset(&g_rp_main_calib_params, 0x03));     // TX MUXIN gain setting
        g_fpga_rb_reg_mem->tx_muxin_src = 0x00000003;                                                      // source ID: 3
      }
      break;
#endif

      case RB_MODSRC_AC97_LINEOUT_L: {
        //fprintf(stderr, "INFO - fpga_rb_set_ctrl: setting FPGA tx_modsrc to AC97 LINEIN Left\n");

        fpga_rb_set_tx_muxin_gain(tx_muxin_gain, 0);                                                       // TX MUXIN gain setting
        g_fpga_rb_reg_mem->tx_muxin_src = 0x00000030;                                                      // source ID: 48
      }
      break;

      case RB_MODSRC_AC97_LINEOUT_R: {
        //fprintf(stderr, "INFO - fpga_rb_set_ctrl: setting FPGA tx_modsrc to AC97 LINEIN Right\n");

        fpga_rb_set_tx_muxin_gain(tx_muxin_gain, 0);                                                       // TX MUXIN gain setting
        g_fpga_rb_reg_mem->tx_muxin_src = 0x00000031;                                                      // source ID: 49
      }
      break;

      }  // switch (tx_modsrc)


      if (tx_modsrc != RB_MODSRC_NONE) {
        fpga_rb_set_tx_modtyp(tx_modtyp);                                                                  // power savings control: set TX modulation variant

        switch (tx_modtyp) {

        case RB_TX_MODTYP_USB: {
          //fprintf(stderr, "INFO - fpga_rb_set_ctrl: setting FPGA for TX: USB\n");

          g_fpga_rb_reg_mem->ctrl &= ~0x00007076;                                                          // TX: turn off all STREAMING, RESET and RESYNC signals
          fpga_rb_set_tx_amp_rf_gain_ofs__4mod_all(tx_amp_rf_gain * 1.5, 0.0);                             // TX_AMP_RF  gain setting [mV] is global and not modulation dependent
          if (tx_car_osc_qrg_inc == 50) {
            fpga_rb_set_tx_car_osc_qrg__4mod_cw_ssb_am_pm(tx_car_osc_qrg + ssb_weaver_osc_qrg);            // TX_CAR_OSC frequency with ssb_weaver_osc_qrg correction
          }
          fpga_rb_set_tx_car_osc_qrg_inc__4mod_cw_ssb_am_pm(tx_car_osc_qrg_inc);                           // TX_CAR_OSC frequency sweep increment

          fpga_rb_set_tx_mod_osc_qrg__4mod_ssbweaver_am_fm_pm(+ssb_weaver_osc_qrg);                        // TX_MOD_OSC weaver method mixer LO frequency
          fpga_rb_set_tx_mod_qmix_gain_ofs__4mod_cw_ssbweaver_am(tx_mod_osc_mag, 0);                       // SSB operation has no carrier
        }
        break;

        case RB_TX_MODTYP_LSB: {
          //fprintf(stderr, "INFO - fpga_rb_set_ctrl: setting FPGA for TX: LSB\n");

          g_fpga_rb_reg_mem->ctrl &= ~0x00007076;                                                          // TX: turn off all STREAMING, RESET and RESYNC signals
          fpga_rb_set_tx_amp_rf_gain_ofs__4mod_all(tx_amp_rf_gain * 1.5, 0.0);                             // TX_AMP_RF  gain setting [mV] is global and not modulation dependent
          if (tx_car_osc_qrg_inc == 50) {
            fpga_rb_set_tx_car_osc_qrg__4mod_cw_ssb_am_pm(tx_car_osc_qrg - ssb_weaver_osc_qrg);            // TX_CAR_OSC frequency with ssb_weaver_osc_qrg correction
          }
          fpga_rb_set_tx_car_osc_qrg_inc__4mod_cw_ssb_am_pm(tx_car_osc_qrg_inc);                           // TX_CAR_OSC frequency sweep increment

          fpga_rb_set_tx_mod_osc_qrg__4mod_ssbweaver_am_fm_pm(-ssb_weaver_osc_qrg);                        // TX_MOD_OSC weaver method mixer LO frequency
          fpga_rb_set_tx_mod_qmix_gain_ofs__4mod_cw_ssbweaver_am(tx_mod_osc_mag, 0);                       // SSB operation has no carrier
        }
        break;

        case RB_TX_MODTYP_AM: {
          //fprintf(stderr, "INFO - fpga_rb_set_ctrl: setting FPGA for TX: AM\n");

          g_fpga_rb_reg_mem->ctrl &= ~0x00007076;                                                          // TX: turn off all STREAMING, RESET and RESYNC signals
          fpga_rb_set_tx_amp_rf_gain_ofs__4mod_all(tx_amp_rf_gain, 0.0);                                   // TX_AMP_RF  gain setting [mV] is global and not modulation dependent
          if (tx_car_osc_qrg_inc == 50) {
            fpga_rb_set_tx_car_osc_qrg__4mod_cw_ssb_am_pm(tx_car_osc_qrg);                                 // TX_CAR_OSC frequency
          }
          fpga_rb_set_tx_car_osc_qrg_inc__4mod_cw_ssb_am_pm(tx_car_osc_qrg_inc);                           // TX_CAR_OSC frequency sweep increment

          if (tx_modsrc == RB_MODSRC_MOD_OSC) {
            fpga_rb_set_tx_mod_osc_qrg__4mod_ssbweaver_am_fm_pm(tx_mod_osc_qrg);                           // TX_MOD_OSC frequency
          } else {
            fpga_rb_set_tx_mod_osc_qrg__4mod_ssbweaver_am_fm_pm(0.0);                                      // TX_MOD_OSC turning off
            if (!(g_fpga_rb_reg_mem->status & 0x00000100)) {
              // TX_MOD_OSC phase not zero: reset phase oscillator
              g_fpga_rb_reg_mem->ctrl &= ~0x00001000;                                                      // TX_MOD RESYNC activate
              g_fpga_rb_reg_mem->ctrl |=  0x00001000;                                                      // TX_MOD RESYNC deactivate
            }
          }

          fpga_rb_set_tx_mod_qmix_gain_ofs__4mod_cw_ssbweaver_am(tx_mod_osc_mag, 1);                       // AM by streaming in amplitude
        }
        break;

        case RB_TX_MODTYP_FM: {
          //fprintf(stderr, "INFO - fpga_rb_set_ctrl: setting FPGA for TX: FM\n");

          g_fpga_rb_reg_mem->ctrl &= ~0x00007056;                                                          // TX: turn off offset STREAMING, RESET and RESYNC signals
          fpga_rb_set_tx_amp_rf_gain_ofs__4mod_all(tx_amp_rf_gain, 0.0);                                   // TX_AMP_RF  gain setting [mV] is global and not modulation dependent
          if (tx_modsrc == RB_MODSRC_MOD_OSC) {  // TODO scanner does not work for TX_MOD_FM
            fpga_rb_set_tx_mod_osc_qrg__4mod_ssbweaver_am_fm_pm(tx_mod_osc_qrg);                           // TX_MOD_OSC frequency
          } else {
            fpga_rb_set_tx_mod_osc_qrg__4mod_ssbweaver_am_fm_pm(0.0);                                      // TX_MOD_OSC turning off
            if (!(g_fpga_rb_reg_mem->status & 0x00000100)) {
              // TX_MOD_OSC phase not zero: reset phase oscillator
              g_fpga_rb_reg_mem->ctrl &= ~0x00001000;                                                      // TX_MOD RESYNC activate
              g_fpga_rb_reg_mem->ctrl |=  0x00001000;                                                      // TX_MOD RESYNC deactivate
            }
          }
          fpga_rb_set_tx_mod_qmix_gain_ofs__4mod_fm(tx_car_osc_qrg, tx_mod_osc_mag);                       // FM by streaming in DDS increment
          g_fpga_rb_reg_mem->ctrl |=  0x00000020;                                                          // control: FM by TX_CAR_OSC increment streaming
        }
        break;

        case RB_TX_MODTYP_PM: {
          //fprintf(stderr, "INFO - fpga_rb_set_ctrl: setting FPGA for TX: PM\n");

          g_fpga_rb_reg_mem->ctrl &= ~0x00007036;                                                          // TX: turn off increment STREAMING, RESET and RESYNC signals
          fpga_rb_set_tx_amp_rf_gain_ofs__4mod_all(tx_amp_rf_gain, 0.0);                                   // TX_AMP_RF  gain setting [mV] is global and not modulation dependent
          if (tx_car_osc_qrg_inc == 50) {
            fpga_rb_set_tx_car_osc_qrg__4mod_cw_ssb_am_pm(tx_car_osc_qrg);                                 // TX_CAR_OSC frequency
          }
          fpga_rb_set_tx_car_osc_qrg_inc__4mod_cw_ssb_am_pm(tx_car_osc_qrg_inc);                           // TX_CAR_OSC frequency sweep increment

          if (tx_modsrc == RB_MODSRC_MOD_OSC) {
            fpga_rb_set_tx_mod_osc_qrg__4mod_ssbweaver_am_fm_pm(tx_mod_osc_qrg);                           // TX_MOD_OSC frequency
          } else {
            fpga_rb_set_tx_mod_osc_qrg__4mod_ssbweaver_am_fm_pm(0.0);                                      // TX_MOD_OSC turning off
            if (!(g_fpga_rb_reg_mem->status & 0x00000100)) {
              // TX_MOD_OSC phase not zero: reset phase oscillator
              g_fpga_rb_reg_mem->ctrl &= ~0x00001000;                                                      // TX_MOD RESYNC activate
              g_fpga_rb_reg_mem->ctrl |=  0x00001000;                                                      // TX_MOD RESYNC deactivate
            }
          }

          fpga_rb_set_tx_mod_qmix_gain_ofs__4mod_pm(tx_car_osc_qrg, tx_mod_osc_mag);                       // PM by streaming in DDS phase offset
          g_fpga_rb_reg_mem->ctrl |=  0x00000040;                                                          // control: PM by TX_CAR_OSC offset streaming
        }
        break;

        default:
          fpga_rb_set_tx_amp_rf_gain_ofs__4mod_all(tx_amp_rf_gain, 0.0);                                   // TX_AMP_RF  gain setting [mV] is global and not modulation dependent
          if (tx_car_osc_qrg_inc == 50) {
            fpga_rb_set_tx_car_osc_qrg__4mod_cw_ssb_am_pm(tx_car_osc_qrg);                                 // CW mode keeps oscillator on QRG frequency
          }
          fpga_rb_set_tx_car_osc_qrg_inc__4mod_cw_ssb_am_pm(tx_car_osc_qrg_inc);                           // CW mode keeps scanner active

          fpga_rb_set_tx_mod_osc_qrg__4mod_ssbweaver_am_fm_pm(0.0);                                        // TX_MOD_OSC turning off
          if (!(g_fpga_rb_reg_mem->status & 0x00000100)) {
            // TX_MOD_OSC phase not zero: reset phase oscillator
            g_fpga_rb_reg_mem->ctrl &= ~0x00001000;                                                        // TX_MOD RESYNC activate
            g_fpga_rb_reg_mem->ctrl |=  0x00001000;                                                        // TX_MOD RESYNC deactivate
          }

        }  // switch (tx_modtyp)
      }  // if (tx_modsrc != RB_MODSRC_NONE)

      // -- 8< --

      switch (rx_muxin_src) {

      default:
      case RB_MODSRC_NONE: {
        //fprintf(stderr, "INFO - fpga_rb_set_ctrl: setting FPGA rx_modsrc to (none)\n");

        fpga_rb_set_rx_muxin_gain(rx_muxin_gain, 0x0000);                                                  // RX MUXIN gain setting
        g_fpga_rb_reg_mem->rx_muxin_src = 0x00000000;
      }
      break;

      case RB_MODSRC_RF_IN1: {
        //fprintf(stderr, "INFO - fpga_rb_set_ctrl: setting FPGA rx_modsrc to RF_inp_1\n");

        fpga_rb_set_rx_muxin_gain(rx_muxin_gain, calib_get_ADC_offset(&g_rp_main_calib_params, 0x20));     // RX MUXIN gain setting
        g_fpga_rb_reg_mem->rx_muxin_src = 0x00000020;
        g_fpga_rb_reg_mem->ctrl |= adc_auto_ofs;                                                           // ADC automatic offset compensation
      }
      break;

      case RB_MODSRC_RF_IN2: {
        //fprintf(stderr, "INFO - fpga_rb_set_ctrl: setting FPGA rx_modsrc to RF_inp_2\n");

        fpga_rb_set_rx_muxin_gain(rx_muxin_gain, calib_get_ADC_offset(&g_rp_main_calib_params, 0x21));     // RX MUXIN gain setting
        g_fpga_rb_reg_mem->rx_muxin_src = 0x00000021;
        g_fpga_rb_reg_mem->ctrl |= adc_auto_ofs;                                                           // ADC automatic offset compensation
      }
      break;

      case RB_MODSRC_EXP_AI0: {
        //fprintf(stderr, "INFO - fpga_rb_set_ctrl: setting FPGA rx_modsrc to AI0\n");

        fpga_rb_set_rx_muxin_gain(rx_muxin_gain, calib_get_ADC_offset(&g_rp_main_calib_params, 0x10));     // RX MUXIN gain setting
        g_fpga_rb_reg_mem->rx_muxin_src = 0x00000010;
        g_fpga_rb_reg_mem->ctrl |= adc_auto_ofs;                                                           // ADC automatic offset compensation
      }
      break;

      case RB_MODSRC_EXP_AI1: {
        //fprintf(stderr, "INFO - fpga_rb_set_ctrl: setting FPGA rx_modsrc to AI1\n");

        fpga_rb_set_rx_muxin_gain(rx_muxin_gain, calib_get_ADC_offset(&g_rp_main_calib_params, 0x18));     // RX MUXIN gain setting
        g_fpga_rb_reg_mem->rx_muxin_src = 0x00000018;
        g_fpga_rb_reg_mem->ctrl |= adc_auto_ofs;                                                           // ADC automatic offset compensation
      }
      break;

      case RB_MODSRC_EXP_AI2: {
        //fprintf(stderr, "INFO - fpga_rb_set_ctrl: setting FPGA rx_modsrc to AI2\n");

        fpga_rb_set_rx_muxin_gain(rx_muxin_gain, calib_get_ADC_offset(&g_rp_main_calib_params, 0x11));     // RX MUXIN gain setting
        g_fpga_rb_reg_mem->rx_muxin_src = 0x00000011;
        g_fpga_rb_reg_mem->ctrl |= adc_auto_ofs;                                                           // ADC automatic offset compensation
      }
      break;

      case RB_MODSRC_EXP_AI3: {
        //fprintf(stderr, "INFO - fpga_rb_set_ctrl: setting FPGA rx_modsrc to AI3\n");

        fpga_rb_set_rx_muxin_gain(rx_muxin_gain, calib_get_ADC_offset(&g_rp_main_calib_params, 0x19));     // RX MUXIN gain setting
        g_fpga_rb_reg_mem->rx_muxin_src = 0x00000019;
        g_fpga_rb_reg_mem->ctrl |= adc_auto_ofs;                                                           // ADC automatic offset compensation
      }
      break;

#if 0
      case RB_MODSRC_VP_VN: {
        fpga_rb_set_rx_muxin_gain(rx_muxin_gain, calib_get_ADC_offset(&g_rp_main_calib_params, 0x03));     // RX MUXIN gain setting
        g_fpga_rb_reg_mem->rx_muxin_src = 0x00000003;
      }
      break;
#endif

      case RB_MODSRC_AC97_LINEOUT_L: {
        //fprintf(stderr, "INFO - fpga_rb_set_ctrl: setting FPGA rx_modsrc to AC97-LINEOUT Left\n");

        fpga_rb_set_rx_muxin_gain(rx_muxin_gain, 0);                                                       // RX MUXIN gain setting
        g_fpga_rb_reg_mem->rx_muxin_src = 0x00000030;
      }
      break;

      case RB_MODSRC_AC97_LINEOUT_R: {
        //fprintf(stderr, "INFO - fpga_rb_set_ctrl: setting FPGA rx_modsrc to AC97-LINEOUT Right\n");

        fpga_rb_set_rx_muxin_gain(rx_muxin_gain, 0);                                                       // RX MUXIN gain setting
        g_fpga_rb_reg_mem->rx_muxin_src = 0x00000031;
      }
      break;

      }  // switch (rx_modsrc)


      fpga_rb_set_rx_modtyp(rx_modtyp & 0x0f);                                                             // power savings control: set RX modulation variant, main part of modulation-type

      switch (rx_modtyp & 0x0f) {

      case RB_RX_MODTYP_USB: {
        //fprintf(stderr, "INFO - fpga_rb_set_ctrl: setting FPGA for RX: USB\n");

        g_fpga_rb_reg_mem->ctrl &= ~0x10760000;                                                            // RX: turn off RX RESET, RESYNC, INCREMENT- and PHASE-STREAMING signals
        if (rx_car_osc_qrg_inc == 50) {
          fpga_rb_set_rx_car_osc_qrg__4mod_ssb_am_fm_pm(rx_car_osc_qrg + ssb_weaver_osc_qrg);              // RX_CAR_OSC frequency with ssb_weaver_osc_qrg correction
        }
        fpga_rb_set_rx_car_osc_qrg_inc__4mod_ssb_am_fm_pm(rx_car_osc_qrg_inc);                             // RX_CAR_OSC frequency sweep increment
        fpga_rb_set_rx_mod_osc_qrg__4mod_ssbweaver_am(+ssb_weaver_osc_qrg);                                // RX_MOD_OSC weaver method mixer LO frequency
      }
      break;

      case RB_RX_MODTYP_LSB: {
        //fprintf(stderr, "INFO - fpga_rb_set_ctrl: setting FPGA for RX: LSB\n");

        g_fpga_rb_reg_mem->ctrl &= ~0x10760000;                                                            // RX: turn off RX RESET, RESYNC, INCREMENT- and PHASE-STREAMING signals
        if (rx_car_osc_qrg_inc == 50) {
          fpga_rb_set_rx_car_osc_qrg__4mod_ssb_am_fm_pm(rx_car_osc_qrg - ssb_weaver_osc_qrg);              // RX_CAR_OSC frequency with ssb_weaver_osc_qrg correction
        }
        fpga_rb_set_rx_car_osc_qrg_inc__4mod_ssb_am_fm_pm(rx_car_osc_qrg_inc);                             // RX_CAR_OSC frequency sweep increment
        fpga_rb_set_rx_mod_osc_qrg__4mod_ssbweaver_am(-ssb_weaver_osc_qrg);                                // RX_MOD_OSC weaver method mixer LO frequency
      }
      break;

      case RB_RX_MODTYP_AMSYNC_USB: {
        //fprintf(stderr, "INFO - fpga_rb_set_ctrl: setting FPGA for RX: AM-SYNC (USB)\n");

        g_fpga_rb_reg_mem->ctrl &= ~0x10560000;                                                            // RX: turn off RX RESET, RESYNC and PHASE-STREAMING signals
        g_fpga_rb_reg_mem->ctrl |=  0x00200000;                                                            // RX: AM-SYNC detection by AFC increment streaming
        if (rx_car_osc_qrg_inc == 50) {
          fpga_rb_set_rx_car_osc_qrg__4mod_ssb_am_fm_pm(rx_car_osc_qrg + ssb_weaver_osc_qrg);              // RX_CAR_OSC frequency with ssb_weaver_osc_qrg correction
        }
        fpga_rb_set_rx_car_osc_qrg_inc__4mod_ssb_am_fm_pm(rx_car_osc_qrg_inc);                             // RX_CAR_OSC frequency sweep increment
        fpga_rb_set_rx_mod_osc_qrg__4mod_ssbweaver_am(+ssb_weaver_osc_qrg);                                // RX_MOD_OSC weaver method mixer LO frequency
        fpga_rb_set_rx_calc_afc_weaver__4mod_am_fm_pm(+ssb_weaver_osc_qrg);                                // RX_CAR_CALC_WEAVER AFC weaver frequency offset correction
      }
      break;

      case RB_RX_MODTYP_AMSYNC_LSB: {
        //fprintf(stderr, "INFO - fpga_rb_set_ctrl: setting FPGA for RX: AM-SYNC (LSB)\n");

        g_fpga_rb_reg_mem->ctrl &= ~0x10560000;                                                            // RX: turn off RX RESET, RESYNC and PHASE-STREAMING signals
        g_fpga_rb_reg_mem->ctrl |=  0x00200000;                                                            // RX: AM-SYNC detection by AFC increment streaming
        if (rx_car_osc_qrg_inc == 50) {
          fpga_rb_set_rx_car_osc_qrg__4mod_ssb_am_fm_pm(rx_car_osc_qrg - ssb_weaver_osc_qrg);              // RX_CAR_OSC frequency with ssb_weaver_osc_qrg correction
        }
        fpga_rb_set_rx_car_osc_qrg_inc__4mod_ssb_am_fm_pm(rx_car_osc_qrg_inc);                             // RX_CAR_OSC frequency sweep increment
        fpga_rb_set_rx_mod_osc_qrg__4mod_ssbweaver_am(-ssb_weaver_osc_qrg);                                // RX_MOD_OSC weaver method mixer LO frequency
        fpga_rb_set_rx_calc_afc_weaver__4mod_am_fm_pm(-ssb_weaver_osc_qrg);                                // RX_CAR_CALC_WEAVER AFC weaver frequency offset correction
      }
      break;

      case RB_RX_MODTYP_FM: {
        //fprintf(stderr, "INFO - fpga_rb_set_ctrl: setting FPGA for RX: FM\n");

        g_fpga_rb_reg_mem->ctrl &= ~0x10560000;                                                            // RX: turn off RX RESET, RESYNC and PHASE-STREAMING signals
        g_fpga_rb_reg_mem->ctrl |=  0x00200000;                                                            // RX: FM detection by AFC increment streaming
        if (rx_car_osc_qrg_inc == 50) {
          fpga_rb_set_rx_car_osc_qrg__4mod_ssb_am_fm_pm(rx_car_osc_qrg);                                   // RX_CAR_OSC frequency
        }
        fpga_rb_set_rx_car_osc_qrg_inc__4mod_ssb_am_fm_pm(rx_car_osc_qrg_inc);                             // RX_CAR_OSC frequency sweep increment
        fpga_rb_set_rx_calc_afc_weaver__4mod_am_fm_pm(0.0);                                                // RX_CAR_CALC_WEAVER AFC weaver frequency offset correction
      }
      break;

      case RB_RX_MODTYP_PM: {
        //fprintf(stderr, "INFO - fpga_rb_set_ctrl: setting FPGA for RX: PM\n");

        g_fpga_rb_reg_mem->ctrl &= ~0x10560000;                                                            // RX: turn off RX RESET, RESYNC and PHASE-STREAMING signals
        g_fpga_rb_reg_mem->ctrl |=  0x00200000;                                                            // RX: PM detection by AFC increment streaming
        if (rx_car_osc_qrg_inc == 50) {
          fpga_rb_set_rx_car_osc_qrg__4mod_ssb_am_fm_pm(rx_car_osc_qrg);                                   // RX_CAR_OSC frequency
        }
        fpga_rb_set_rx_car_osc_qrg_inc__4mod_ssb_am_fm_pm(rx_car_osc_qrg_inc);                             // RX_CAR_OSC frequency sweep increment
        fpga_rb_set_rx_calc_afc_weaver__4mod_am_fm_pm(0.0);                                                // RX_CAR_CALC_WEAVER AFC weaver frequency offset correction
      }
      break;

      case RB_RX_MODTYP_AMENV: {
        //fprintf(stderr, "INFO - fpga_rb_set_ctrl: setting FPGA for RX: AM-ENV\n");

        g_fpga_rb_reg_mem->ctrl &= ~0x10560000;                                                            // RX: turn off RX RESET, RESYNC and PHASE-STREAMING signals
        g_fpga_rb_reg_mem->ctrl |=  0x00200000;                                                            // RX: AM-ENV detection by AFC increment streaming
        if (rx_car_osc_qrg_inc == 50) {
          fpga_rb_set_rx_car_osc_qrg__4mod_ssb_am_fm_pm(rx_car_osc_qrg);                                   // RX_CAR_OSC frequency
        }
        fpga_rb_set_rx_car_osc_qrg_inc__4mod_ssb_am_fm_pm(rx_car_osc_qrg_inc);                             // RX_CAR_OSC frequency sweep increment
        fpga_rb_set_rx_calc_afc_weaver__4mod_am_fm_pm(0.0);                                                // RX_CAR_CALC_WEAVER AFC weaver frequency offset correction
        fpga_rb_set_rx_amenv_filtvar(rx_modtyp >> 4);                                                      // RB_RX_EMENV_FILT_VARIANT set RX_AFC_FIR filter to given characteristics
      }
      break;

      default:
        fpga_rb_set_rx_car_osc_qrg__4mod_ssb_am_fm_pm(0.0);                                                // no need for oscillator to run
        fpga_rb_set_rx_car_osc_qrg_inc__4mod_ssb_am_fm_pm(50);                                             // no need for oscillator to scan

      }  // switch (rx_modtyp & 0x0f)


    } else {  // else if (rb_run)
      g_fpga_rb_reg_mem->ctrl &= ~0x10767076;                                                              // TX/RX: turn off all STREAMING, RESET and RESYNC signals
      g_fpga_rb_reg_mem->tx_muxin_src = 0x00000000;                                                        // TX_MUXIN input off
      fpga_rb_set_tx_amp_rf_gain_ofs__4mod_all(0.0, 0.0);                                                  // TX_AMP_RF gain/offset control
      //fpga_rb_set_tx_car_osc_qrg__4mod_cw_ssb_am_pm(0.0);                                                // do not loose current frequency of TX_CAR_OSC
      fpga_rb_set_tx_car_osc_qrg_inc__4mod_cw_ssb_am_pm(50);                                               // TX_CAR_OSC frequency sweep increment, mid-range
      fpga_rb_set_tx_mod_osc_qrg__4mod_ssbweaver_am_fm_pm(0.0);                                            // TX_MOD_OSC frequency
      fpga_rb_set_tx_mod_qmix_gain_ofs__4mod_fm(0.0f, 0.0);                                                // TX_MOD_QMIX gain/offset control
      g_fpga_rb_reg_mem->rx_muxin_src = 0;                                                                 // RX_MUX input off
      //fpga_rb_set_rx_car_osc_qrg__4mod_ssb_am_fm_pm(0.0);                                                // do not loose current frequency of RX_CAR_OSC frequency
      fpga_rb_set_rx_car_osc_qrg_inc__4mod_ssb_am_fm_pm(50);                                               // RX_CAR_OSC frequency sweep increment, mid-range
      fpga_rb_set_rx_calc_afc_weaver__4mod_am_fm_pm(0.0);                                                  // RX_CAR_CALC_WEAVER frequency
      fpga_rb_set_rx_mod_osc_qrg__4mod_ssbweaver_am(0.0);                                                  // RX_MOD_OSC frequency

      //fpga_rb_reset();                                                                                   // control: turn off all streams into TX_CAR_OSC and TX_CAR_OSC mixer
    }
}


/*----------------------------------------------------------------------------*/
void fpga_rb_get_ctrl(int tx_modtyp, int rx_modtyp,
        double* loc_RD_tx_car_osc_qrg, double* loc_RD_rx_car_osc_qrg, uint16_t* loc_RD_ovrdrv)
{
    const int ssb_weaver_osc_qrg = 1700.0;
    //fprintf(stderr, "DEBUG - fpga_rb_get_ctrl: requesting current FPGA data ...\n");

    *loc_RD_tx_car_osc_qrg = fpga_rb_get_tx_car_osc_qrg();
    switch (tx_modtyp) {
    case RB_TX_MODTYP_USB:
      *loc_RD_tx_car_osc_qrg -= ssb_weaver_osc_qrg;
      break;

    case RB_TX_MODTYP_LSB:
      *loc_RD_tx_car_osc_qrg += ssb_weaver_osc_qrg;
      break;
    }

    *loc_RD_rx_car_osc_qrg = fpga_rb_get_rx_car_osc_qrg();
    switch (rx_modtyp) {
    case RB_RX_MODTYP_USB:
    case RB_RX_MODTYP_AMSYNC_USB:
      *loc_RD_rx_car_osc_qrg -= ssb_weaver_osc_qrg;
      break;

    case RB_TX_MODTYP_LSB:
    case RB_RX_MODTYP_AMSYNC_LSB:
      *loc_RD_rx_car_osc_qrg += ssb_weaver_osc_qrg;
      break;
    }

    *loc_RD_ovrdrv = fpga_rb_get_ovrdrv();
    //fprintf(stderr, "DEBUG - fpga_rb_get_ctrl: ... tx_car_osc_qrg = %lf, rx_car_osc_qrg = %lf, ovrdrv = %d\n", *loc_RD_tx_car_osc_qrg, *loc_RD_rx_car_osc_qrg, *loc_RD_ovrdrv);
}


/*----------------------------------------------------------------------------*/
void fpga_rb_set_tx_modtyp(int tx_modtyp)
{
    int tx = tx_modtyp & 0xff;
    uint32_t masked = g_fpga_rb_reg_mem->pwr_ctrl & 0xffff00ff;
    g_fpga_rb_reg_mem->pwr_ctrl = masked;                                                                     // first disable and reset before entering new modulation variant
    g_fpga_rb_reg_mem->pwr_ctrl = masked | (tx << 0x08);
}


/*----------------------------------------------------------------------------*/
void fpga_rb_set_tx_muxin_gain(int tx_muxin_gain, int tx_muxin_ofs)
{
    double p;

    if (tx_muxin_gain > 100) {
        tx_muxin_gain = 100;
    }

    if (tx_muxin_gain <= 0) {
        g_fpga_rb_reg_mem->tx_muxin_gain = 0;
        //fprintf(stderr, "INFO - fpga_rb_set_tx_muxin_gain: ZERO   tx_muxin_gain=%d --> bitfield=0x%08x\n", tx_muxin_gain, g_fpga_rb_reg_mem->tx_muxin_gain);

    } else if (tx_muxin_gain < 80) {  // 0% .. 80%-
        uint32_t bitfield = (uint32_t) (0.5 + (tx_muxin_gain * ((double) 0xffff) / 80.0));
        g_fpga_rb_reg_mem->tx_muxin_gain = (0xffff & bitfield);  // 16 bit gain value and no booster shift bits
        //fprintf(stderr, "INFO - fpga_rb_set_tx_muxin_gain: NORMAL tx_muxin_gain=%d --> bitfield=0x%08x\n", tx_muxin_gain, g_fpga_rb_reg_mem->tx_muxin_gain);

    } else {  // 80% .. 100%: set the logarithmic amplifier
        p  = (tx_muxin_gain - 80) * (7.0 / 20.0);
        uint32_t bitfield = (uint32_t) (0.5 + p);
        g_fpga_rb_reg_mem->tx_muxin_gain = ((bitfield << 16) | 0xffff);  // open mixer completely and activate booster
        //fprintf(stderr, "INFO - fpga_rb_set_tx_muxin_gain: BOOST  tx_muxin_gain=%d --> bitfield=0x%08x\n", tx_muxin_gain, g_fpga_rb_reg_mem->tx_muxin_gain);
    }

    g_fpga_rb_reg_mem->tx_muxin_ofs = tx_muxin_ofs & 0xffff;
}

/*----------------------------------------------------------------------------*/
void fpga_rb_set_tx_mod_osc_qrg__4mod_ssbweaver_am_fm_pm(double tx_mod_osc_qrg)
{
    double qrg = ((double) (1ULL << 48)) * (tx_mod_osc_qrg / g_rp_main_calib_params.base_osc125mhz_realhz);
    if (qrg > 0.0) {
        qrg += 0.5;
    } else if (qrg < 0.0) {
        qrg -= -0.5;
    }

    int64_t bitfield = qrg;
    uint32_t bf_hi = (uint32_t) (bitfield >> 32);
    uint32_t bf_lo = (uint32_t) (bitfield & 0xffffffff);

    //fprintf(stderr, "INFO - fpga_rb_set_tx_mod_osc_qrg__4mod_ssbweaver_am_fm_pm: (qrg=%lf, HI=0x%08x, LO=0x%08x) <-- in(tx_mod_osc_qrg=%lf)\n",
    //        qrg,
    //        bf_hi,
    //        bf_lo,
    //        tx_mod_osc_qrg);

    g_fpga_rb_reg_mem->tx_mod_osc_inc_lo = bf_lo;
    g_fpga_rb_reg_mem->tx_mod_osc_inc_hi = bf_hi;
    g_fpga_rb_reg_mem->tx_mod_osc_ofs_lo = 0UL;                                                            // no carrier phase offset
    g_fpga_rb_reg_mem->tx_mod_osc_ofs_hi = 0UL;                                                            // no carrier phase offset
}

/*----------------------------------------------------------------------------*/
void fpga_rb_set_tx_mod_qmix_gain_ofs__4mod_cw_ssbweaver_am(double tx_mod_qmix_grade, int isOffset)
{
    double gain, ofs;

    if (isOffset) {  // CW and AM modulation: reduced modulation by 1/2 and added offset to reach the maximum at the modulation peaks
        gain = ((double) 0x7fff) * (tx_mod_qmix_grade / 100.0);
        ofs  = ((double) ((1ULL << 47) - 1)) - (((double) ((1ULL << 46) - 1)) * (tx_mod_qmix_grade / 100.0));

    } else {  // SSB modulation: no offset but full modulation
        gain = ((double) 0xffff) * (tx_mod_qmix_grade / 100.0);
        ofs  = 0.0;
    }

    //fprintf(stderr, "INFO - fpga_rb_set_tx_mod_qmix_gain_ofs__4mod_cw_am: (gain=%lf, ofs=%lf) <-- in(tx_mod_qmix_grade=%lf)\n",
    //        gain, ofs, tx_mod_qmix_grade);

    g_fpga_rb_reg_mem->tx_mod_qmix_gain   = ((uint32_t) gain) & 0xffff;
    g_fpga_rb_reg_mem->tx_mod_qmix_ofs_lo = (uint32_t) (((uint64_t) ofs)  & 0xffffffff);                   // CW, and AM have carrier enabled,
    g_fpga_rb_reg_mem->tx_mod_qmix_ofs_hi = (uint32_t) (((uint64_t) ofs) >> 32);                           // SSB is zero symmetric w/o a carrier
}

/*----------------------------------------------------------------------------*/
void fpga_rb_set_tx_mod_qmix_gain_ofs__4mod_fm(double tx_car_osc_qrg, double tx_mod_osc_mag)
{
    double gain = ((double) 0xffff) * ((1UL << 14) * tx_mod_osc_mag / g_rp_main_calib_params.base_osc125mhz_realhz);
    double ofs  = ((double) (1ULL << 48)) * (tx_car_osc_qrg / g_rp_main_calib_params.base_osc125mhz_realhz);
    //fprintf(stderr, "INFO - fpga_rb_set_tx_mod_qmix_gain_ofs__4mod_fm: (gain=%lf, ofs=%lf) <-- in(tx_car_osc_qrg=%lf, tx_mod_osc_mag=%lf)\n",
    //        gain, ofs, tx_car_osc_qrg, tx_mod_osc_mag);

    g_fpga_rb_reg_mem->tx_mod_qmix_gain   = ((uint32_t) gain) & 0xffff;                                    // FM deviation
    g_fpga_rb_reg_mem->tx_mod_qmix_ofs_lo = (uint32_t) (((uint64_t) ofs)  & 0xffffffff);                   // FM carrier frequency
    g_fpga_rb_reg_mem->tx_mod_qmix_ofs_hi = (uint32_t) (((uint64_t) ofs) >> 32);
}

/*----------------------------------------------------------------------------*/
void fpga_rb_set_tx_mod_qmix_gain_ofs__4mod_pm(double tx_car_osc_qrg, double tx_mod_osc_mag)
{
    double gain = ((double) 0xffff) * (tx_mod_osc_mag / 180.0);
    //fprintf(stderr, "INFO - fpga_rb_set_tx_mod_osc_mixer_mod_pm: tx_car_osc_qrg=%lf, tx_mod_osc_mag=%lf\n",
    //        tx_car_osc_qrg, tx_mod_osc_mag);

    g_fpga_rb_reg_mem->tx_mod_qmix_gain   = ((uint32_t) gain) & 0xffff;                                    // PM phase magnitude
    g_fpga_rb_reg_mem->tx_mod_qmix_ofs_lo = 0UL;                                                           // PM based on zero phase w/o modulation
    g_fpga_rb_reg_mem->tx_mod_qmix_ofs_hi = 0UL;
}

/*----------------------------------------------------------------------------*/
void fpga_rb_set_tx_car_osc_qrg__4mod_cw_ssb_am_pm(double tx_car_osc_qrg)
{
    double qrg = ((double) (1ULL << 48)) * (tx_car_osc_qrg / g_rp_main_calib_params.base_osc125mhz_realhz);
    if (qrg > 0.0) {
        qrg += 0.5;
    } else if (qrg < 0.0) {
        qrg -= -0.5;
    }

    int64_t bitfield = qrg;
    uint32_t bf_hi = (uint32_t) (bitfield >> 32);
    uint32_t bf_lo = (uint32_t) (bitfield & 0xffffffff);

    //fprintf(stderr, "INFO - fpga_rb_set_tx_car_osc_qrg__4mod_cw_ssb_am_pm: (qrg=%lf, HI=0x%08x, LO=0x%08x) <-- in(tx_car_osc_qrg=%lf)\n",
    //        qrg,
    //        bf_hi,
    //        bf_lo,
    //        tx_car_osc_qrg);

    g_fpga_rb_reg_mem->tx_car_osc_inc_lo = bf_lo;
    g_fpga_rb_reg_mem->tx_car_osc_inc_hi = bf_hi;
    g_fpga_rb_reg_mem->tx_car_osc_ofs_lo = 0UL;                                                            // no carrier phase offset
    g_fpga_rb_reg_mem->tx_car_osc_ofs_hi = 0UL;                                                            // no carrier phase offset
}

/*----------------------------------------------------------------------------*/
double fpga_rb_get_tx_car_osc_qrg()
{
    int64_t bitfield;
    bitfield  =            g_fpga_rb_reg_mem->tx_car_osc_inc_lo;
    bitfield |= ((int64_t) g_fpga_rb_reg_mem->tx_car_osc_inc_hi) << 32;

    double tx_car_osc_qrg = g_rp_main_calib_params.base_osc125mhz_realhz * (((double) bitfield) / ((double) (1ULL << 48)));
    tx_car_osc_qrg = floor(tx_car_osc_qrg + 0.5);
    //fprintf(stderr, "INFO - fpga_rb_get_tx_car_osc_qrg: read tx_car_osc_qrg = %f, bitfield = %lld\n", tx_car_osc_qrg, bitfield);
    return tx_car_osc_qrg;
}

/*----------------------------------------------------------------------------*/
void fpga_rb_set_tx_car_osc_qrg_inc__4mod_cw_ssb_am_pm(int tx_car_osc_qrg_inc)
{
    const double maxDev   = 10000.0;
    const double dynamic  = 3.0;
    double       rngctrlr = ((tx_car_osc_qrg_inc - 50) / 50.0);  // [-1.0 .. +1.0]
    int          neg      = 0;

    if (fabs(rngctrlr) < 0.1) {  // middle-range is inactive  +/-10 %
        //fprintf(stderr, "INFO - fpga_rb_set_tx_car_osc_qrg_inc__4mod_cw_ssb_am_pm: STOPPING SCANNER <-- mid-range\n");
        g_fpga_rb_reg_mem->tx_car_osc_inc_scnr_lo = 0;
        g_fpga_rb_reg_mem->tx_car_osc_inc_scnr_hi = 0;
        return;
    }

    if (rngctrlr < 0.0) {
        rngctrlr = -rngctrlr;
        neg = 1;
    }

    rngctrlr *= dynamic;
    rngctrlr -= dynamic;  // [-dynamic .. 0]

    double hzPerSec = maxDev * pow(10.0, rngctrlr);

    int64_t bitfield = hzPerSec * ((1ULL << 48) / g_rp_main_calib_params.base_osc125mhz_realhz) / 200e3;
    if (neg) {
        bitfield = ~bitfield;
    }
    uint32_t bf_hi = (uint32_t) (bitfield >> 32);
    uint32_t bf_lo = (uint32_t) (bitfield & 0xffffffff);

    //fprintf(stderr, "INFO - fpga_rb_set_tx_car_osc_qrg_inc__4mod_cw_ssb_am_pm: (hzPerSec=%lf, bitfield=%lld, HI=0x%08x, LO=0x%08x) <-- in(tx_car_osc_qrg_inc=%d%%)\n",
    //        hzPerSec,
    //        bitfield,
    //        bf_hi,
    //        bf_lo,
    //        tx_car_osc_qrg_inc);

    g_fpga_rb_reg_mem->tx_car_osc_inc_scnr_lo = bf_lo;
    g_fpga_rb_reg_mem->tx_car_osc_inc_scnr_hi = bf_hi;
}

/*----------------------------------------------------------------------------*/
double fpga_rb_get_tx_car_osc_qrg_inc()
{
    int neg            = 0;
    int64_t bitfield   =            g_fpga_rb_reg_mem->tx_car_osc_inc_scnr_lo;
    bitfield          |= ((int64_t) g_fpga_rb_reg_mem->tx_car_osc_inc_scnr_hi) << 32;
    if (!bitfield) {
        return 0.0;
    }

    if (bitfield < 0) {
        neg = 1;
        bitfield = ~bitfield;
    }

    double tx_car_osc_qrg_inc = (((double) bitfield) * g_rp_main_calib_params.base_osc125mhz_realhz * 200e3) / ((double) (1ULL << 48));
    if (neg) {
        tx_car_osc_qrg_inc = -tx_car_osc_qrg_inc;
    }
    //fprintf(stderr, "INFO - fpga_rb_get_tx_car_osc_qrg_inc: read tx_car_osc_qrg_inc = %f Hz/s, bitfield=%lld\n", tx_car_osc_qrg_inc, bitfield);
    return tx_car_osc_qrg_inc;
}

/*----------------------------------------------------------------------------*/
void fpga_rb_set_tx_amp_rf_gain_ofs__4mod_all(double tx_amp_rf_gain, double tx_amp_rf_ofs)
{
    double gain = ((double) 0x7fff) * (tx_amp_rf_gain / 2048.0);
    double ofs  = ((double) 0x7fff) * (tx_amp_rf_ofs  / 2048.0);

    //fprintf(stderr, "INFO - fpga_rb_set_tx_amp_rf_gain_ofs__4mod_all: (gain=%lf, ofs=%lf) <-- in(tx_amp_rf_gain=%lf, tx_amp_rf_ofs=%lf)\n",
    //        gain, ofs, tx_amp_rf_gain, tx_amp_rf_ofs);

    g_fpga_rb_reg_mem->tx_amp_rf_gain = ((uint32_t) gain) & 0xffff;
    g_fpga_rb_reg_mem->tx_amp_rf_ofs  = ((uint32_t) ofs)  & 0xffff;
}


/*----------------------------------------------------------------------------*/
void fpga_rb_set_rx_modtyp(int rx_modtyp)
{
    int rx = rx_modtyp & 0xff;
    uint32_t masked = g_fpga_rb_reg_mem->pwr_ctrl & 0xffffff00;
    g_fpga_rb_reg_mem->pwr_ctrl = masked;                                                                     // first disable and reset before entering new modulation variant
    g_fpga_rb_reg_mem->pwr_ctrl = masked | rx;
}

/*----------------------------------------------------------------------------*/
void fpga_rb_set_rx_muxin_gain(int rx_muxin_gain, int rx_muxin_ofs)
{
    double p;

    if (rx_muxin_gain > 100) {
        rx_muxin_gain = 100;
    }

    if (rx_muxin_gain <= 0) {
        g_fpga_rb_reg_mem->rx_muxin_gain = 0;
        //fprintf(stderr, "INFO - fpga_rb_set_rx_muxin_gain: ZERO   rx_muxin_gain=%d --> bitfield=0x%08x\n", rx_muxin_gain, g_fpga_rb_reg_mem->rx_muxin_gain);

    } else if (rx_muxin_gain < 80) {  // 0% .. 80%-
        uint32_t bitfield = (uint32_t) (0.5 + (rx_muxin_gain * ((double) 0xffff) / 80.0));
        g_fpga_rb_reg_mem->rx_muxin_gain = (0xffff & bitfield);  // 16 bit gain value and no booster shift bits
        //fprintf(stderr, "INFO - fpga_rb_set_rx_muxin_gain: NORMAL rx_muxin_gain=%lf --> bitfield=0x%08x\n", rx_muxin_gain, g_fpga_rb_reg_mem->rx_muxin_gain);

    } else {  // 80% .. 100%: set the logarithmic amplifier
        p  = (rx_muxin_gain - 80) * (5.0 / 20.0);
        uint32_t bitfield = (uint32_t) (0.5 + p);
        g_fpga_rb_reg_mem->rx_muxin_gain = ((bitfield << 16) | 0xffff);  // open mixer completely and activate booster
        //fprintf(stderr, "INFO - fpga_rb_set_rx_muxin_gain: BOOST  rx_muxin_gain=%d --> bitfield=0x%08x\n", rx_muxin_gain, g_fpga_rb_reg_mem->rx_muxin_gain);
    }

    g_fpga_rb_reg_mem->rx_muxin_ofs = rx_muxin_ofs & 0xffff;
}

/*----------------------------------------------------------------------------*/
void fpga_rb_set_rx_car_osc_qrg__4mod_ssb_am_fm_pm(double rx_car_osc_qrg)
{
    double qrg = ((double) (1ULL << 48)) * (rx_car_osc_qrg / g_rp_main_calib_params.base_osc125mhz_realhz);
    if (qrg > 0.0) {
        qrg += 0.5;
    } else if (qrg < 0.0) {
        qrg -= -0.5;
    }

    int64_t bitfield = qrg;
    uint32_t bf_hi = (uint32_t) (bitfield >> 32);
    uint32_t bf_lo = (uint32_t) (bitfield & 0xffffffff);

    //fprintf(stderr, "INFO - fpga_rb_set_rx_car_osc_qrg__4mod_ssb_am_fm_pm: (qrg=%lf, HI=0x%08x, LO=0x%08x) <-- in(rx_car_osc_qrg=%lf)\n",
    //        qrg,
    //        bf_hi,
    //        bf_lo,
    //        rx_car_osc_qrg);

    g_fpga_rb_reg_mem->rx_car_osc_inc_lo = bf_lo;
    g_fpga_rb_reg_mem->rx_car_osc_inc_hi = bf_hi;
    g_fpga_rb_reg_mem->rx_car_osc_ofs_lo = 0UL;                                                            // no carrier phase offset
    g_fpga_rb_reg_mem->rx_car_osc_ofs_hi = 0UL;                                                            // no carrier phase offset
}

/*----------------------------------------------------------------------------*/
double fpga_rb_get_rx_car_osc_qrg()
{
    int64_t bitfield;
    bitfield  =            g_fpga_rb_reg_mem->rx_car_osc_inc_lo;
    bitfield |= ((int64_t) g_fpga_rb_reg_mem->rx_car_osc_inc_hi) << 32;

    double rx_car_osc_qrg = g_rp_main_calib_params.base_osc125mhz_realhz * (((double) bitfield) / ((double) (1ULL << 48)));
    rx_car_osc_qrg = floor(rx_car_osc_qrg + 0.5);
    //fprintf(stderr, "INFO - fpga_rb_get_rx_car_osc_qrg: read rx_car_osc_qrg = %f, bitfield = %lld\n", rx_car_osc_qrg, bitfield);
    return rx_car_osc_qrg;
}

/*----------------------------------------------------------------------------*/
void fpga_rb_set_rx_car_osc_qrg_inc__4mod_ssb_am_fm_pm(int rx_car_osc_qrg_inc)
{
    const double maxDev   = 10000.0;
    const double dynamic  = 3.0;
    double       rngctrlr = ((rx_car_osc_qrg_inc - 50) / 50.0);  // [-1.0 .. +1.0]
    int          neg      = 0;

    if (fabs(rngctrlr) < 0.1) {  // middle-range is inactive  +/-10 %
        //fprintf(stderr, "INFO - fpga_rb_set_rx_car_osc_qrg_inc__4mod_ssb_am_fm_pm: STOPPING SCANNER <-- mid-range\n");
        g_fpga_rb_reg_mem->rx_car_osc_inc_scnr_lo = 0;
        g_fpga_rb_reg_mem->rx_car_osc_inc_scnr_hi = 0;
        return;
    }

    if (rngctrlr < 0.0) {
        rngctrlr = -rngctrlr;
        neg = 1;
    }

    rngctrlr *= dynamic;
    rngctrlr -= dynamic;  // [-dynamic .. 0]

    double hzPerSec = maxDev * pow(10.0, rngctrlr);

    int64_t bitfield = (hzPerSec * (1ULL << 48) / g_rp_main_calib_params.base_osc125mhz_realhz) / 200e3;
    if (neg) {
        bitfield = ~bitfield;
    }
    uint32_t bf_hi = (uint32_t) (bitfield >> 32);
    uint32_t bf_lo = (uint32_t) (bitfield & 0xffffffff);

    //fprintf(stderr, "INFO - fpga_rb_set_rx_car_osc_qrg_inc__4mod_ssb_am_fm_pm: (hzPerSec=%lf, bitfield=%lld, HI=0x%08x, LO=0x%08x) <-- in(rx_car_osc_qrg_inc=%d%%)\n",
    //        hzPerSec,
    //        bitfield,
    //        bf_hi,
    //        bf_lo,
    //        rx_car_osc_qrg_inc);

    g_fpga_rb_reg_mem->rx_car_osc_inc_scnr_lo = bf_lo;
    g_fpga_rb_reg_mem->rx_car_osc_inc_scnr_hi = bf_hi;
}

/*----------------------------------------------------------------------------*/
double fpga_rb_get_rx_car_osc_qrg_inc()
{
    int neg            = 0;
    int64_t bitfield   =            g_fpga_rb_reg_mem->rx_car_osc_inc_scnr_lo;
    bitfield          |= ((int64_t) g_fpga_rb_reg_mem->rx_car_osc_inc_scnr_hi) << 32;
    if (!bitfield) {
        return 0.0;
    }

    if (bitfield < 0) {
        neg = 1;
        bitfield = ~bitfield;
    }

    double rx_car_osc_qrg_inc = (((double) bitfield) * g_rp_main_calib_params.base_osc125mhz_realhz * 200e3) / ((double) (1ULL << 48));
    if (neg) {
        rx_car_osc_qrg_inc = -rx_car_osc_qrg_inc;
    }
    //fprintf(stderr, "INFO - fpga_rb_get_rx_car_osc_qrg_inc: read rx_car_osc_qrg_inc = %f Hz/s, bitfield=%lld\n", rx_car_osc_qrg_inc, bitfield);
    return rx_car_osc_qrg_inc;
}

/*----------------------------------------------------------------------------*/
void fpga_rb_set_rx_mod_osc_qrg__4mod_ssbweaver_am(double rx_mod_osc_qrg)
{
    double qrg = ((double) (1ULL << 48)) * (rx_mod_osc_qrg / g_rp_main_calib_params.base_osc125mhz_realhz);
    if (qrg > 0.0) {
        qrg += 0.5;
    } else if (qrg < 0.0) {
        qrg -= -0.5;
    }

    /* RX_MOD_OSC DDS increment value calculation */
    int64_t bitfield = qrg;
    uint32_t bf_hi = (uint32_t) (bitfield >> 32);
    uint32_t bf_lo = (uint32_t) (bitfield & 0xffffffff);

    //fprintf(stderr, "INFO - fpga_rb_set_rx_mod_osc_qrg__4mod_ssbweaver_am_fm: MOD offset INC - (qrg=%lf, HI=0x%08x, LO=0x%08x) <-- in(rx_mod_osc_qrg=%lf)\n",
    //        qrg,
    //        bf_hi,
    //        bf_lo,
    //        rx_mod_osc_qrg);

    g_fpga_rb_reg_mem->rx_mod_osc_inc_lo = bf_lo;
    g_fpga_rb_reg_mem->rx_mod_osc_inc_hi = bf_hi;
    g_fpga_rb_reg_mem->rx_mod_osc_ofs_lo = 0UL;                                                            // no carrier phase offset
    g_fpga_rb_reg_mem->rx_mod_osc_ofs_hi = 0UL;                                                            // no carrier phase offset
}

/*----------------------------------------------------------------------------*/
void fpga_rb_set_rx_calc_afc_weaver__4mod_am_fm_pm(double rx_weaver_qrg)
{
    double qrg = ((double) (1ULL << 48)) * (rx_weaver_qrg / g_rp_main_calib_params.base_osc125mhz_realhz);
    if (qrg > 0.0) {
        qrg += 0.5;
    } else if (qrg < 0.0) {
        qrg -= -0.5;
    }

    /* AFC weaver offset correction - phase correction value integrated for a 200 kHz = 5 µs time span */
    qrg *= -625.0;
    int64_t bitfield = qrg;
    uint32_t bf_hi = (uint32_t) (bitfield >> 32);
    uint32_t bf_lo = (uint32_t) (bitfield & 0xffffffff);

    //fprintf(stderr, "INFO - fpga_rb_set_rx_calc_afc_weaver__4mod_am_fm_pm: AFC correction - (qrg=%lf, HI=0x%08x, LO=0x%08x) <-- in(rx_car_osc AFC weaver phase correction=%lf)\n",
    //        qrg,
    //        bf_hi,
    //        bf_lo,
    //        rx_weaver_qrg);

    g_fpga_rb_reg_mem->rx_car_calc_weaver_inc_lo = bf_lo;
    g_fpga_rb_reg_mem->rx_car_calc_weaver_inc_hi = bf_hi;
}

/*----------------------------------------------------------------------------*/
void fpga_rb_set_rx_amenv_filtvar(int rx_amenv_filtvar)
{
    if (rx_amenv_filtvar < 0)
        rx_amenv_filtvar = 0;
    else if (rx_amenv_filtvar > 2)
        rx_amenv_filtvar = 2;

    g_fpga_rb_reg_mem->rx_amenv_filtvar = ((uint32_t) rx_amenv_filtvar) & 0x0003;
}

/*----------------------------------------------------------------------------*/
void fpga_rb_set_rx_mod_ssb_am_gain__4mod_ssb_am(double rx_mod_ssb_am_gain)
{
    double gain = ((double) 0xffff) * 0.5 * (rx_mod_ssb_am_gain / 100.0);

    //fprintf(stderr, "INFO - fpga_rb_set_rx_mod_ssb_am_gain__4mod_ssb_am: (gain=%lf) <-- in(rx_mod_ssb_am_gain=%lf)\n",
    //        gain, rx_mod_ssb_am_gain);

    g_fpga_rb_reg_mem->rx_mod_ssb_am_gain = ((uint32_t) gain) & 0xffff;
}

/*----------------------------------------------------------------------------*/
void fpga_rb_set_rx_mod_amenv_gain__4mod_amenv(double rx_mod_amenv_gain)
{
    double gain = ((double) 0xffff) * (rx_mod_amenv_gain / 100.0);

    //fprintf(stderr, "INFO - fpga_rb_set_rx_mod_amenv_gain__4mod_amenv: (gain=%lf) <-- in(rx_mod_amenv_gain=%lf)\n",
    //        gain, rx_mod_amenv_gain);

    g_fpga_rb_reg_mem->rx_mod_amenv_gain = ((uint32_t) gain) & 0xffff;
}

/*----------------------------------------------------------------------------*/
void fpga_rb_set_rx_mod_fm_gain__4mod_fm(double rx_mod_fm_gain)
{
    double gain = ((double) 0xffff) * (rx_mod_fm_gain / 100.0);

    //fprintf(stderr, "INFO - fpga_rb_set_rx_mod_fm_gain__4mod_fm: (gain=%lf) <-- in(rx_mod_fm_gain=%lf)\n",
    //        gain, rx_mod_fm_gain);

    g_fpga_rb_reg_mem->rx_mod_fm_gain = ((uint32_t) gain) & 0xffff;
}

/*----------------------------------------------------------------------------*/
void fpga_rb_set_rx_mod_pm_gain__4mod_pm(double rx_mod_pm_gain)
{
    double gain = ((double) 0xffff) * (rx_mod_pm_gain / 100.0);

    //fprintf(stderr, "INFO - fpga_rb_set_rx_mod_pm_gain__4mod_pm: (gain=%lf) <-- in(rx_mod_pm_gain=%lf)\n",
    //        gain, rx_mod_pm_gain);

    g_fpga_rb_reg_mem->rx_mod_pm_gain = ((uint32_t) gain) & 0xffff;
}

/*----------------------------------------------------------------------------*/
void fpga_rb_set_rfout1_gain_ofs(double rfout1_gain, uint16_t rfout1_ofs)
{
    if (!rfout1_gain) {  // no output gain correction --> multiply with 1.0000
        rfout1_gain = 1.0;
    }
    double gain = ((double) 0x0100) * rfout1_gain;  // 8 bit integer . 8 bit fractional part

    //fprintf(stderr, "INFO - fpga_rb_set_rfout1_gain_ofs: (gain=0x%08x) <-- in(rfout1_gain=%lf, rfout1_ofs=%d)\n",
    //                (uint32_t) gain, rfout1_gain, rfout1_ofs);

    g_fpga_rb_reg_mem->rfout1_gain = ((uint32_t) gain) & 0xffff;
    g_fpga_rb_reg_mem->rfout1_ofs  = rfout1_ofs;
}

/*----------------------------------------------------------------------------*/
void fpga_rb_set_rfout2_gain_ofs(double rfout2_gain, uint16_t rfout2_ofs)
{
    if (!rfout2_gain) {  // no output gain correction --> multiply with 1.0000
        rfout2_gain = 1.0;
    }
    double gain = ((double) 0x0100) * rfout2_gain;  // 8 bit integer . 8 bit fractional part

    //fprintf(stderr, "INFO - fpga_rb_set_rfout2_gain_ofs: (gain=0x%08x) <-- in(rfout2_gain=%lf, rfout2_ofs=%d)\n",
    //                (uint32_t) gain, rfout2_gain, rfout2_ofs);

    g_fpga_rb_reg_mem->rfout2_gain = ((uint32_t) gain) & 0xffff;
    g_fpga_rb_reg_mem->rfout2_ofs  = rfout2_ofs;
}

/*----------------------------------------------------------------------------*/
uint16_t fpga_rb_get_ovrdrv()
{
    uint16_t ovrdrv = (g_fpga_rb_reg_mem->status & 0x00C00000) >> 0x16;
    //fprintf(stderr, "INFO - fpga_rb_get_ovrdrv: ovrdrv = 0x%02x\n", ovrdrv);
    return ovrdrv;
}


/* --------------------------------------------------------------------------- *
 * CALIBRATION
 * --------------------------------------------------------------------------- */

void prepare_rx_measurement(int inputLine)
{
    // enable RB
    g_fpga_rb_reg_mem->ctrl = 0x00000001;

    // power up the RX_CAR and RX_AFC section like for modulation FM (broad RX_AFC_FIR filter)
    g_fpga_rb_reg_mem->pwr_ctrl = 0x00000007;

    // keep all output silent
    g_fpga_rb_reg_mem->src_con_pnt = 0x00000000;

    // RX_OSC set to 10 kHz
    g_fpga_rb_reg_mem->rx_car_osc_inc_lo = 0x3e2d6238;
    g_fpga_rb_reg_mem->rx_car_osc_inc_hi = 0x00000005;

    // select input line
    //fprintf(stderr, "\nDEBUG prepare_rx_measurement: preparing ADC channel 0x%02x\n", inputLine);
    g_fpga_rb_reg_mem->rx_muxin_src = inputLine;

    // set the input gain to maximum but no boost enabled
    g_fpga_rb_reg_mem->rx_muxin_gain = 0x00001fff;
}

void finish_rx_measurement()
{
    // clear the input offset register
    g_fpga_rb_reg_mem->rx_muxin_ofs = 0x00000000;

    // close input line
    g_fpga_rb_reg_mem->rx_muxin_src = 0;

    // RX_OSC clear
    g_fpga_rb_reg_mem->rx_car_osc_inc_lo = 0;
    g_fpga_rb_reg_mem->rx_car_osc_inc_hi = 0;

    // no power savings enabled
    g_fpga_rb_reg_mem->pwr_ctrl = 0x00000000;

    // disable RB
    g_fpga_rb_reg_mem->ctrl = 0;
}

uint32_t test_rx_measurement(int16_t adc_offset_val, int reduction)
{
    uint32_t sumreg = 0;

    // set the ADC offset value
    g_fpga_rb_reg_mem->rx_muxin_ofs = adc_offset_val;

    g_fpga_rb_reg_mem->rx_muxin_gain = 0x0000ffff >> reduction;

    // delay for filters going to be stable
    {
        struct timespec rqtp;

        // each 200 kHz timestamp there is a new result available - sum up to reduce noise during measurement
        rqtp.tv_sec  = 0;
        rqtp.tv_nsec = 5000L;
        nanosleep(&rqtp, NULL);

        int iter;
        for (iter = 8; iter; --iter) {
            sumreg += ((g_fpga_rb_reg_mem->rx_afc_cordic_mag + 16) >> 5);  // each part is rounded
            nanosleep(&rqtp, NULL);
        }
    }

    // read the current magnitude value of the CORDIC engine
    return sumreg;
}

int16_t rp_minimize_noise()
{
    // binary search algorithm

    uint16_t min_ofs_value = 0x0000;
    uint16_t test_ofs_lo;
    uint16_t test_ofs_hi;
    uint32_t test_sig_lo;
    uint32_t test_sig_hi;

    int i;
    for (i = 15; i > 0; --i) {
        int reduction = i - 11;
        if (reduction < 0) {
            reduction = 0;
        }

        test_ofs_lo = min_ofs_value | (0b01 << (i - 1));
        test_ofs_hi = min_ofs_value | (0b11 << (i - 1));

        test_sig_lo = test_rx_measurement((int16_t) (((int32_t) test_ofs_lo) - 0x8000), reduction);
        test_sig_hi = test_rx_measurement((int16_t) (((int32_t) test_ofs_hi) - 0x8000), reduction);
        //fprintf(stderr, "DEBUG rp_minimize_noise: i=%02d, test_ofs_lo=0x%04x sig=%08x - test_ofs_hi=0x%04x sig=%08x\n", i, test_ofs_lo, test_sig_lo, test_ofs_hi, test_sig_hi);
        if (test_sig_hi < test_sig_lo) {
            min_ofs_value |= (1 << i);
        }
    }
    test_ofs_lo = min_ofs_value;
    test_ofs_hi = min_ofs_value | 0b1;
    test_sig_lo = test_rx_measurement((int16_t) (((int32_t) test_ofs_lo) - 0x8000), 0);
    test_sig_hi = test_rx_measurement((int16_t) (((int32_t) test_ofs_hi) - 0x8000), 0);
    //fprintf(stderr, "DEBUG rp_minimize_noise: i=%02d, test_ofs_lo=0x%04x sig=%08x - test_ofs_hi=0x%04x sig=%08x\n", 0, test_ofs_lo, test_sig_lo, test_ofs_hi, test_sig_hi);
    if (test_sig_hi < test_sig_lo) {
        min_ofs_value = test_ofs_hi;
    }

    //fprintf(stderr, "INFO rp_minimize_noise: FINAL --> min_ofs_value=0x%04x=%d\n", min_ofs_value, min_ofs_value);
    return (int16_t) (((int32_t) min_ofs_value) - 0x8000);
}

void rp_measure_calib_params(rp_calib_params_t* calib_params)
{
    int16_t thisOfsValue;

    fprintf(stderr, "\n<== ADC offset calibration ==>\n");

    // measure offset of channel ADC0 mapped to "RF In 1"
    prepare_rx_measurement(0x20);
    thisOfsValue = rp_minimize_noise();
    fprintf(stderr, "INFO rp_measure_calib_params:  ADC channel 0 - ofs=0x%04x = %d\n", thisOfsValue, thisOfsValue);
    calib_set_ADC_offset(calib_params, 0x20, thisOfsValue);

    // measure offset of channel ADC1 mapped to "RF In 2"
    prepare_rx_measurement(0x21);
    thisOfsValue = rp_minimize_noise();
    fprintf(stderr, "INFO rp_measure_calib_params:  ADC channel 1 - ofs=0x%04x = %d\n", thisOfsValue, thisOfsValue);
    calib_set_ADC_offset(calib_params, 0x21, thisOfsValue);

    // measure offset of XADC channel #8 mapped to "Vin0"
    prepare_rx_measurement(0x18);
    thisOfsValue = rp_minimize_noise();
    fprintf(stderr, "INFO rp_measure_calib_params: XADC channel 0 - ofs=0x%04x = %d\n", thisOfsValue, thisOfsValue);
    calib_set_ADC_offset(calib_params, 0x18, thisOfsValue);

    // measure offset of XADC channel #0 mapped to "Vin1"
    prepare_rx_measurement(0x10);
    thisOfsValue = rp_minimize_noise();
    fprintf(stderr, "INFO rp_measure_calib_params: XADC channel 1 - ofs=0x%04x = %d\n", thisOfsValue, thisOfsValue);
    calib_set_ADC_offset(calib_params, 0x10, thisOfsValue);

    // measure offset of XADC channel #1 mapped to "Vin2"
    prepare_rx_measurement(0x11);
    thisOfsValue = rp_minimize_noise();
    fprintf(stderr, "INFO rp_measure_calib_params: XADC channel 2 - ofs=0x%04x = %d\n", thisOfsValue, thisOfsValue);
    calib_set_ADC_offset(calib_params, 0x11, thisOfsValue);

    // measure offset of XADC channel #9 mapped to "Vin3"
    prepare_rx_measurement(0x19);
    thisOfsValue = rp_minimize_noise();
    fprintf(stderr, "INFO rp_measure_calib_params: XADC channel 3 - ofs=0x%04x = %d\n", thisOfsValue, thisOfsValue);
    calib_set_ADC_offset(calib_params, 0x19, thisOfsValue);

#if 0
    // measure offset of XADC channel Vp/Vn mapped to "Vin4"
    prepare_rx_measurement(0x03);
    thisOfsValue = rp_minimize_noise();
    fprintf(stderr, "INFO rp_measure_calib_params: XADC channel 4 - ofs=0x%04x = %d\n", thisOfsValue, thisOfsValue);
    calib_set_ADC_offset(calib_params, 0x03, thisOfsValue);
#endif

    finish_rx_measurement();
    fprintf(stderr, "\n");
}


#if 0
/* --------------------------------------------------------------------------- *
 * FPGA SECOND ACCESS METHOD
 * --------------------------------------------------------------------------- */

/*----------------------------------------------------------------------------*/
/**
 * @brief Reads value from the specific RadioBox sub-module register
 *
 * @param[in] rb_reg_ofs  offset value for the RadioBox base address to be written to.
 *
 * @retval  value of the specified register.
 */
uint32_t fpga_rb_read_register(unsigned int rb_reg_ofs)
{
    //fprintf(stderr, "fpga_rb_read_register: BEGIN\n");
    if (!g_fpga_rb_reg_mem) {
        return -1;
    }

    uint32_t value = *((uint32_t*) ((void*) g_fpga_rb_reg_mem) + rb_reg_ofs);
    fprintf(stderr, "fpga_rb_read_register: ofs=0x%06x --> read=0x%08x\n", rb_reg_ofs, value);
    fprintf(stderr, "fpga_rb_read_register: END\n");
    return value;
}

/*----------------------------------------------------------------------------*/
/**
 * @brief Writes value to the specific RadioBox sub-module register
 *
 * @param[in] rb_reg_ofs  offset value for the RadioBox base address to be written to.
 * @param[in] value  value that is written to the specified register.
 *
 * @retval  0 Success
 * @retval -1 Failure, error message is output on standard error device
 */
int fpga_rb_write_register(unsigned int rb_reg_ofs, uint32_t value)
{
    //fprintf(stderr, "fpga_rb_write_register: BEGIN\n");

    if (!g_fpga_rb_reg_mem) {
        return -1;
    }

    //fprintf(stderr, "INFO fpga_rb_write_register: Compare LED access: %p, calced=%p\n", &(g_fpga_rb_reg_mem->src_con_pnt), ((void*) g_fpga_rb_reg_mem) + rb_reg_ofs);

    fprintf(stderr, "fpga_rb_write_register: ofs=0x%06x <-- write=0x%08x\n", rb_reg_ofs, value);
    *((uint32_t*) ((void*) g_fpga_rb_reg_mem) + rb_reg_ofs) = value;

    //fprintf(stderr, "fpga_rb_write_register: END\n");
    return 0;
}
#endif
