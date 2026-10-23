/**
 * @file bsp_usb.c
 * @author your name (you@domain.com)
 * @brief usb是单例bsp,因此不保存实例
 * @version 0.2
 * @date 2023-02-09
 *
 * @copyright Copyright (c) 2023
 *
 */

#include <string.h>

#include "bsp_usb.h"
#include "bsp_log.h"
#include "bsp_tools.h"

#if ((USB_RX_RING_SIZE == 0U) || ((USB_RX_RING_SIZE & (USB_RX_RING_SIZE - 1U)) != 0U))
#error "USB_RX_RING_SIZE must be a power of two"
#endif

#if ((USB_TX_RING_SIZE == 0U) || ((USB_TX_RING_SIZE & (USB_TX_RING_SIZE - 1U)) != 0U))
#error "USB_TX_RING_SIZE must be a power of two"
#endif

#define USB_CRITICAL_BASEPRI 0x50U
#define USB_RING_MASK(size) ((size) - 1U)

extern USBD_HandleTypeDef hUsbDeviceFS;

static uint8_t usb_rx_ring[USB_RX_RING_SIZE];
static volatile uint16_t usb_rx_head;
static volatile uint16_t usb_rx_tail;
static volatile uint8_t usb_rx_overflow;
static volatile uint32_t usb_rx_drop_count;
static volatile uint8_t usb_rx_enabled;

static uint8_t usb_tx_ring[USB_TX_RING_SIZE];
static volatile uint16_t usb_tx_head;
static volatile uint16_t usb_tx_count;
static volatile uint16_t usb_tx_in_flight;
static volatile uint32_t usb_tx_drop_count;

static USBCallback usb_rx_cbk = NULL;
static USBCallback usb_tx_cbk = NULL;
static CallbackTaskHandle_t usb_rx_task;

static inline uint32_t USBCriticalEnter(void)
{
    uint32_t saved_baspri = __get_BASEPRI();
    __set_BASEPRI(USB_CRITICAL_BASEPRI);
    return saved_baspri;
}

static inline void USBCriticalExit(uint32_t saved_baspri)
{
    __set_BASEPRI(saved_baspri);
}

static void USBTxStart(void)
{
    uint16_t seg_len;
    uint8_t *seg_buf;

    uint32_t saved_baspri = USBCriticalEnter();

    if ((usb_tx_in_flight != 0U) || (usb_tx_count == 0U))
    {
        USBCriticalExit(saved_baspri);
        return;
    }

    if ((hUsbDeviceFS.pClassData == NULL) || (hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED))
    {
        USBCriticalExit(saved_baspri);
        return;
    }

    seg_len = USB_TX_RING_SIZE - usb_tx_head;
    if (seg_len > usb_tx_count)
        seg_len = usb_tx_count;

    usb_tx_count -= seg_len;
    usb_tx_in_flight = seg_len;
    seg_buf = &usb_tx_ring[usb_tx_head];

    USBCriticalExit(saved_baspri);

    if (CDC_Transmit_FS(seg_buf, seg_len) != USBD_OK)
    {
        saved_baspri = USBCriticalEnter();
        usb_tx_in_flight = 0U;
        usb_tx_count += seg_len;
        USBCriticalExit(saved_baspri);
    }
}

static void USBTxComplete(uint16_t len)
{
    uint16_t sent;
    uint32_t saved_baspri = USBCriticalEnter();

    sent = usb_tx_in_flight;
    if (sent != 0U)
    {
        usb_tx_head = (usb_tx_head + sent) & USB_RING_MASK(USB_TX_RING_SIZE);
        usb_tx_in_flight = 0U;
    }

    USBCriticalExit(saved_baspri);

    if (sent == 0U)
    {
        USBTxStart();
        return;
    }

    USBTxStart();

    if (usb_tx_cbk != NULL)
        usb_tx_cbk(len);
}

uint8_t USBTransmit(uint8_t *buffer, uint16_t len)
{
    uint16_t tail;
    uint16_t first;
    uint16_t used;

    if ((buffer == NULL) || (len == 0U) || (len > USB_TX_RING_SIZE))
    {
        usb_tx_drop_count++;
        return 1U;
    }

    uint32_t saved_baspri = USBCriticalEnter();

    used = usb_tx_count + usb_tx_in_flight;
    if ((USB_TX_RING_SIZE - used) < len)
    {
        usb_tx_drop_count++;
        USBCriticalExit(saved_baspri);
        return 1U;
    }

    tail = (usb_tx_head + used) & USB_RING_MASK(USB_TX_RING_SIZE);
    first = USB_TX_RING_SIZE - tail;
    if (first > len)
        first = len;

    memcpy(&usb_tx_ring[tail], buffer, first);
    if (len > first)
        memcpy(&usb_tx_ring[0], &buffer[first], len - first);

    usb_tx_count += len;

    USBCriticalExit(saved_baspri);

    USBTxStart();
    return 0U;
}

uint16_t USBAvailable(void)
{
    return (usb_rx_head - usb_rx_tail) & USB_RING_MASK(USB_RX_RING_SIZE);
}

uint16_t USBRead(uint8_t *buffer, uint16_t len)
{
    uint16_t head;
    uint16_t tail;
    uint16_t available;
    uint16_t first;

    if ((buffer == NULL) || (len == 0U))
        return 0U;

    head = usb_rx_head;
    tail = usb_rx_tail;
    available = (head - tail) & USB_RING_MASK(USB_RX_RING_SIZE);

    if (len > available)
        len = available;
    if (len == 0U)
        return 0U;

    first = USB_RX_RING_SIZE - tail;
    if (first > len)
        first = len;

    memcpy(buffer, &usb_rx_ring[tail], first);
    if (len > first)
        memcpy(&buffer[first], &usb_rx_ring[0], len - first);

    __DMB();
    usb_rx_tail = (tail + len) & USB_RING_MASK(USB_RX_RING_SIZE);

    return len;
}

uint32_t USBGetRxDropCount(void)
{
    return usb_rx_drop_count;
}

uint32_t USBGetTxDropCount(void)
{
    return usb_tx_drop_count;
}

static void USBDeferredRx(void *arg)
{
    (void)arg;

    if (usb_rx_overflow != 0U)
    {
        uint16_t head = usb_rx_head;
        uint16_t discarded = (head - usb_rx_tail) & USB_RING_MASK(USB_RX_RING_SIZE);

        usb_rx_overflow = 0U;
        usb_rx_tail = head;

        if (discarded != 0U)
            LOGWARNING("[bsp_usb] rx overflow, discarded %u bytes", (unsigned int)discarded);

        if (usb_rx_cbk != NULL)
            usb_rx_cbk(0U);
        return;
    }

    if (usb_rx_cbk != NULL)
        usb_rx_cbk(USBAvailable());
}

void USBRxEvent(uint8_t *buffer, uint32_t len)
{
    uint16_t head;
    uint16_t tail;
    uint16_t used;
    uint16_t first;

    if ((usb_rx_enabled == 0U) || (buffer == NULL) || (len == 0U))
        return;

    if (len > USB_RX_RING_SIZE)
    {
        usb_rx_overflow = 1U;
        usb_rx_drop_count++;
        CallbackTaskNotify(&usb_rx_task);
        return;
    }

    head = usb_rx_head;
    tail = usb_rx_tail;
    used = (head - tail) & USB_RING_MASK(USB_RX_RING_SIZE);

    if ((USB_RX_RING_SIZE - used) < len)
    {
        usb_rx_overflow = 1U;
        usb_rx_drop_count++;
        CallbackTaskNotify(&usb_rx_task);
        return;
    }

    first = USB_RX_RING_SIZE - head;
    if (first > len)
        first = (uint16_t)len;

    memcpy(&usb_rx_ring[head], buffer, first);
    if (len > first)
        memcpy(&usb_rx_ring[0], &buffer[first], len - first);

    __DMB();
    usb_rx_head = (head + (uint16_t)len) & USB_RING_MASK(USB_RX_RING_SIZE);

    CallbackTaskNotify(&usb_rx_task);
}

void USBInit(USB_Init_Config_s usb_conf)
{
    uint32_t saved_baspri = USBCriticalEnter();

    usb_rx_head = 0U;
    usb_rx_tail = 0U;
    usb_rx_overflow = 0U;
    usb_rx_drop_count = 0U;
    usb_rx_enabled = 0U;

    usb_tx_head = 0U;
    usb_tx_count = 0U;
    usb_tx_in_flight = 0U;
    usb_tx_drop_count = 0U;

    USBCriticalExit(saved_baspri);

    usb_rx_cbk = usb_conf.rx_cbk;
    usb_tx_cbk = usb_conf.tx_cbk;

    if (usb_rx_cbk != NULL)
    {
        usb_rx_task = CreateCallbackTask("usb_rx", USBDeferredRx, NULL,
                                         osPriorityAboveNormal, CALLBACK_TASK_DEFAULT_STACK_SIZE);
        if (usb_rx_task.thread_id == NULL)
            LOGERROR("[bsp_usb] failed to create rx callback task");
        else
            usb_rx_enabled = 1U;
    }

    CDCInitRxbufferNcallback(USBTxComplete, NULL);
    LOGINFO("USB init success");
}
