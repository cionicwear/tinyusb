/*
 * ehci_controller.c
 *
 *  Created on: Mar 9, 2013
 *      Author: hathach
 */

/*
 * Software License Agreement (BSD License)
 * Copyright (c) 2012, hathach (tinyusb.org)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the tiny usb stack.
 */

//--------------------------------------------------------------------+
// INCLUDE
//--------------------------------------------------------------------+
#include "unity.h"
#include "tusb_option.h"
#include "errors.h"
#include "binary.h"
#include "hal.h"
#include "ehci.h"
#include "usbh_hcd.h"


//--------------------------------------------------------------------+
// MACRO CONSTANT TYPEDEF
//--------------------------------------------------------------------+
ehci_data_t ehci_data;
LPC_USB0_Type lpc_usb0;
LPC_USB1_Type lpc_usb1;

extern usbh_device_info_t usbh_devices[TUSB_CFG_HOST_DEVICE_MAX+1];

//--------------------------------------------------------------------+
// IMPLEMENTATION
//--------------------------------------------------------------------+
void ehci_controller_control_xfer_proceed(uint8_t dev_addr, uint8_t p_data[])
{
  ehci_registers_t* const regs = get_operational_register( usbh_devices[dev_addr].core_id );
  ehci_qhd_t * p_qhd = get_control_qhd(dev_addr);
  ehci_qtd_t * p_qtd_setup = get_control_qtds(dev_addr);
  ehci_qtd_t * p_qtd_data  = p_qtd_setup + 1;
  ehci_qtd_t * p_qtd_status = p_qtd_setup + 2;

  tusb_std_request_t const *p_request = (tusb_std_request_t *) p_qtd_setup->buffer[0];

  if (p_request->wLength > 0 && p_request->bmRequestType.direction == TUSB_DIR_DEV_TO_HOST)
  {
    memcpy(p_qtd_data, p_data, p_request->wLength);
  }

  //------------- retire all QTDs -------------//
  p_qtd_setup->active = p_qtd_data->active = p_qtd_status->active = 0;
  p_qhd->qtd_overlay = *p_qtd_status;

  regs->usb_sts = EHCI_INT_MASK_NXP_ASYNC | EHCI_INT_MASK_NXP_PERIODIC;

  hcd_isr( usbh_devices[dev_addr].core_id );
}

bool complete_all_qtd_in_list(ehci_qhd_t *head)
{
  ehci_qhd_t *p_qhd = head;

  do
  {
    if ( !p_qhd->qtd_overlay.halted )
    {
      while(!p_qhd->qtd_overlay.next.terminate)
      {
        ehci_qtd_t* p_qtd = (ehci_qtd_t*) align32(p_qhd->qtd_overlay.next.address);
        p_qtd->active = 0;
        p_qhd->qtd_overlay = *p_qtd;
      }
    }
    if (!p_qhd->next.terminate)
    {
      p_qhd = (ehci_qhd_t*) align32(p_qhd->next.address);
    }
    else
    {
      break;
    }
  }while(p_qhd != head); // stop if loop around

  return true;
}

void ehci_controller_run(uint8_t hostid)
{
  //------------- Async List -------------//
  ehci_registers_t* const regs = get_operational_register(hostid);
  complete_all_qtd_in_list((ehci_qhd_t*) regs->async_list_base);

  //------------- Period List -------------//
  complete_all_qtd_in_list( get_period_head(hostid) );
  regs->usb_sts = EHCI_INT_MASK_NXP_ASYNC | EHCI_INT_MASK_NXP_PERIODIC;

  hcd_isr(hostid);
}

void ehci_controller_run_error(uint8_t hostid)
{
  //------------- Async List -------------//
  ehci_registers_t* const regs = get_operational_register(hostid);

  ehci_qhd_t *p_qhd = (ehci_qhd_t*) regs->async_list_base;
  do
  {
    if ( !p_qhd->qtd_overlay.halted )
    {
      if(!p_qhd->qtd_overlay.next.terminate)
      {
        ehci_qtd_t* p_qtd = (ehci_qtd_t*) align32(p_qhd->qtd_overlay.next.address);
        p_qtd->active = 0;
        p_qtd->babble_err = p_qtd->buffer_err = p_qtd->xact_err = 1;
        p_qhd->qtd_overlay = *p_qtd;
      }
    }
    p_qhd = (ehci_qhd_t*) align32(p_qhd->next.address);
  }while(p_qhd != get_async_head(hostid)); // stop if loop around
  //------------- Period List -------------//

  regs->usb_sts = EHCI_INT_MASK_ERROR;

  hcd_isr(hostid);
}

void ehci_controller_device_plug(uint8_t hostid, tusb_speed_t speed)
{
  ehci_registers_t* const regs = get_operational_register(hostid);

  regs->usb_sts_bit.port_change_detect    = 1;
  regs->portsc_bit.connect_status_change  = 1;
  regs->portsc_bit.current_connect_status = 1;
  regs->portsc_bit.nxp_port_speed         = speed;

  hcd_isr(hostid);
}

void ehci_controller_device_unplug(uint8_t hostid)
{
  ehci_registers_t* const regs = get_operational_register(hostid);

  regs->usb_sts_bit.port_change_detect    = 1;
  regs->portsc_bit.connect_status_change  = 1;
  regs->portsc_bit.current_connect_status = 0;

  hcd_isr(hostid);
}
