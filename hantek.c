#include <hantek.h>
#include <hantek_priv.h>
#include <hantek_usb.h>

#include <hantek_hexdump.h>

#include <libusb.h>

#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static
HRESULT _hantek_device_find(libusb_device **pdev)
{
    HRESULT ret = H_OK;

    libusb_device **devs = NULL;
    libusb_device *dev = NULL;
    ssize_t nr_devs = 0;

    HASSERT_ARG(NULL != pdev);

    *pdev = NULL;

    libusb_init(NULL);

    if (0 >= (nr_devs = libusb_get_device_list(NULL, &devs))) {
        DEBUG("Failed to get libusb device list, aborting (reason: %zd).", nr_devs);
        ret = H_ERR_NOT_FOUND;
        goto done;
    }

    DEBUG("There are %zd devices connected to the system, walking the list.", nr_devs);

    for (size_t i = 0; i < (size_t)nr_devs; i++) {
        libusb_device *ndev = devs[i];
        struct libusb_device_descriptor desc;

        if (0 != libusb_get_device_descriptor(ndev, &desc)) {
            DEBUG("Failed to get device descriptor at device ID %zu, skipping", i);
            continue;
        }

        if (desc.idVendor != HANTEK_VID) {
            continue;
        }

        if (desc.idProduct != HANTEK_PID_6254BD) {
            DEBUG("Got a Hantek device, PID = %04x", desc.idProduct);
            continue;
        }

        DEBUG("Found a candidate device, %p, breaking the loop", ndev);
        dev = ndev;
        break;
    }

    if (NULL == dev) {
        DEBUG("No supported devices found, aborting.");
        ret = H_ERR_NOT_FOUND;
        goto done;
    }

    libusb_ref_device(dev);
    *pdev = dev;

done:
    if (NULL != devs) {
        libusb_free_device_list(devs, 1);
    }
    return ret;
}

static
HRESULT _hantek_check_ready(libusb_device_handle *dev, bool *pready)
{
	HRESULT ret = H_OK;

    int uret = 0;
    uint8_t raw[10] = { 0 };

    HASSERT_ARG(NULL != dev);
    HASSERT_ARG(NULL != pready);

    *pready = false;

    if (0 >= (uret = libusb_control_transfer(dev,
                    LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
                    HT_REQUEST_CHECK_READY,
                    0x0,
                    0x0,
                    raw,
                    sizeof(raw),
                    0)))
    {
        DEBUG("Failed to request if we are ready for commands, aborting. (reason = %d)", uret);
        ret = H_ERR_CONTROL_FAIL;
        goto done;
    }

    if (uret >= 1) {
        if (raw[0] == 0x1) {
            *pready = true;
        }
    }

done:
    return ret;
}

static
HRESULT _hantek_bulk_cmd_out(libusb_device_handle *dev, uint8_t *data, size_t len, size_t *ptransferred)
{
    HRESULT ret = H_OK;

    uint8_t cmd_start[10] = { 0x0f, 0x03, 0x03, 0x03 };
    int uret = -1,
        transferred = 0;
    bool ready = false;

    HASSERT_ARG(NULL != dev);
    HASSERT_ARG(NULL != data);
    HASSERT_ARG(0 != len);
    HASSERT_ARG(NULL != ptransferred);

    *ptransferred = 0;

    DEBUG("Sending %zu bytes command, sending %zu bytes of prelude", len, sizeof(cmd_start));

    /* Send the magical "start of transaction" command */
    if (0 >= (uret = libusb_control_transfer(dev,
                    LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
                    HT_REQUEST_INITIALIZE,
                    0x0,
                    0x0,
                    cmd_start,
                    sizeof(cmd_start),
                    0)))
    {
        DEBUG("Failed to request if data is ready, aborting. (reason = %d)", uret);
        ret = H_ERR_CONTROL_FAIL;
        goto done;
    }

    /* Check if the device is ready (I think) */
    if (H_FAILED(ret = _hantek_check_ready(dev, &ready))) {
        DEBUG("Was unable to check if ready, aborting.");
        goto done;
    }

    if (false == ready) {
        DEBUG("Device is not ready, aborting.");
        ret = H_ERR_CONTROL_FAIL;
        goto done;
    }

    /* Send the actual bulk command */
    if (0 != (uret = libusb_bulk_transfer(dev,
                                          HT6000_EP_OUT,
                                          data,
                                          len,
                                          &transferred,
                                          0)))
    {
        DEBUG("Failure during bulk OUT transfer: %d", uret);
        ret = H_ERR_NOT_READY;
        goto done;
    }

    DEBUG("Transferred OUT %d bytes, expected %zu", transferred, len);

    *ptransferred = transferred;

done:
    return ret;
}

static
HRESULT _hantek_bulk_in(libusb_device_handle *hdl, uint8_t *data, size_t buf_len, size_t *ptransferred)
{
    HRESULT ret = H_OK;

    int transferred = 0,
        uret = 0;

    HASSERT_ARG(NULL != hdl);
    HASSERT_ARG(NULL != data);
    HASSERT_ARG(0 != buf_len);
    HASSERT_ARG(NULL != ptransferred);

    if (0 != (uret = libusb_bulk_transfer(hdl,
                                          HT6000_EP_IN | (1 << 7),
                                          data,
                                          buf_len,
                                          &transferred,
                                          0)))
    {
        DEBUG("Failure during bulk IN transfer: %d", uret);
        ret = H_ERR_NOT_READY;
        goto done;
    }

    DEBUG("Transferred IN %d bytes, expected %zu", transferred, buf_len);

    *ptransferred = transferred;
done:
    return ret;
}

static
HRESULT _hantek_read_config_attrs(libusb_device_handle *hdl, uint16_t value, void *dst, size_t len_bytes)
{
    HRESULT ret = H_OK;

    int uret = 0;

    HASSERT_ARG(NULL != hdl);
    HASSERT_ARG(NULL != dst);
    HASSERT_ARG(0 != len_bytes);

    if (0 >= (uret = libusb_control_transfer(hdl,
                    LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
                    HT_REQUEST_GET_INFO,
                    value,
                    0x0,
                    dst,
                    len_bytes,
                    0)))
    {
        DEBUG("Failed to request device attribute %04x, (uret = %d), aborting.", value, uret);
        ret = H_ERR_CONTROL_FAIL;
        goto done;
    }

    DEBUG("! Read back %04x: %d bytes (expected = %zu)", value, uret, len_bytes);

done:
    return ret;
}

static
HRESULT _hantek_get_id_string(libusb_device_handle *dev, char *id_string, size_t len)
{
    HRESULT ret = H_OK;

    HASSERT_ARG(NULL != id_string);
    HASSERT_ARG(0 != len);

    if (H_FAILED(ret = _hantek_read_config_attrs(dev, HT_VALUE_GET_INFO_STRING, id_string, len))) {
        DEBUG("Failed to read back ID string, aborting.");
        goto done;
    }

done:
    return ret;
}

static
HRESULT _hantek_device_reset(libusb_device_handle *hdl)
{
    HRESULT ret = H_OK;

    static uint8_t reset_cmd[2] = { HT_MSG_INITIALIZE, 0x0 };
    size_t transferred = 0;

    DEBUG("----> Sending device reset");

    HASSERT_ARG(NULL != hdl);

    if (H_FAILED(ret = _hantek_bulk_cmd_out(hdl, reset_cmd, sizeof(reset_cmd), &transferred))) {
        DEBUG("Failed to send the reset command");
        goto done;
    }

    if (2 != transferred) {
        DEBUG("Transferred %zu bytes, expected 2", transferred);
        goto done;
    }

done:
    return ret;
}

static
HRESULT _hantek_wake_device(libusb_device_handle *hdl)
{
    HRESULT ret = H_OK;

    static
    uint8_t wake_cmds[5][8] = {
        { 0x08, 0x00, 0x00, 0x77, 0x47, 0x12, 0x04, 0x00 },
        { 0x08, 0x00, 0x00, 0x03, 0x00, 0x33, 0x04, 0x00 },
        { 0x08, 0x00, 0x00, 0x65, 0x00, 0x30, 0x02, 0x00 },
        { 0x08, 0x00, 0x00, 0x28, 0xF1, 0x0F, 0x02, 0x00 },
        { 0x08, 0x00, 0x00, 0x12, 0x38, 0x01, 0x02, 0x00 }
    };

    HASSERT_ARG(NULL != hdl);

    for (size_t i = 0; i < 5; i++) {
        size_t transferred = 0;

        if (H_FAILED(ret = _hantek_bulk_cmd_out(hdl, wake_cmds[i], 8, &transferred))) {
            DEBUG("Failure while transmitting wakeup command %zu", i);
            goto done;
        }

        if (transferred != 8) {
            DEBUG("Transferred %zu bytes, expected 8 for a wakeup command", transferred);
            goto done;
        }
    }

done:
    return ret;
}

static
HRESULT _hantek_get_fpga_version(libusb_device_handle *hdl, uint16_t *pfpga_ver)
{
    HRESULT ret = H_OK;

    size_t transferred = 0,
           to_transfer = 0x40;
    bool ready = false;
    uint8_t rx_buf[512];

    HASSERT_ARG(NULL != hdl);
    HASSERT_ARG(NULL != pfpga_ver);

    *pfpga_ver = 0;

    if (H_FAILED(ret = _hantek_check_ready(hdl, &ready))) {
        goto done;
    }

    if (true == ready) {
        DEBUG("Using a large buffer to get the FPGA version");
        to_transfer = 512;
    }

    if (H_FAILED(ret = _hantek_bulk_in(hdl, rx_buf, to_transfer, &transferred))) {
        DEBUG("Failure while getting attributes from the oscilloscope, aborting.");
        goto done;
    }

    if (transferred != to_transfer) {
        DEBUG("Expected to receive %zu, got %zu, aborting.", to_transfer, transferred);
        goto done;
    }

    /* Extract the FPGA version ID */
    *pfpga_ver = (uint16_t)rx_buf[0] << 8 | rx_buf[1];

    DEBUG("FPGA version: %04x", *pfpga_ver);

done:
    return ret;
}

static
HRESULT _hantek_get_calibration_data(libusb_device_handle *hdl, uint16_t *cal_data, size_t nr_cal_vals)
{
    HRESULT ret = H_OK;

    HASSERT_ARG(NULL != hdl);
    HASSERT_ARG(NULL != cal_data);
    HASSERT_ARG(0 != nr_cal_vals);

    if (H_FAILED(ret = _hantek_read_config_attrs(hdl, HT_VALUE_GET_CALIBRATION_DAT, cal_data, sizeof(uint16_t) * nr_cal_vals))) {
        DEBUG("Failed to read back calibration values, aborting.");
        goto done;
    }

done:
    return ret;
}

HRESULT hantek_open_device(struct hantek_device **pdev)
{
    HRESULT ret = H_OK;

    libusb_device *dev = NULL;
    libusb_device_handle *hdl = NULL;
    struct hantek_device *nhdev = NULL;
    int uret = -1;

    HASSERT_ARG(NULL != pdev);

    *pdev = NULL;

    if (H_FAILED(ret = _hantek_device_find(&dev))) {
        DEBUG("Failed to find device, aborting.");
        goto done;
    }

    if (0 != (uret = libusb_open(dev, &hdl))) {
        DEBUG("Failed to open USB device, aborting. Uret = %u)", (unsigned)uret);
        ret = H_ERR_CANT_OPEN;
        goto done;
    }

    /* Claim interface 0 */
    if (0 != (uret = libusb_claim_interface(hdl, 0))) {
        DEBUG("Failed to claim interface 0, aborting. uret = %u", (unsigned)uret);
        ret = H_ERR_CANT_OPEN;
        goto done;
    }

    if (NULL == (nhdev = calloc(1, sizeof(struct hantek_device)))) {
        DEBUG("Out of memory");
        ret = H_ERR_NO_MEM;
        goto done;
    }

    nhdev->dev = dev;
    nhdev->hdl = hdl;

    /* Send what looks to be a reset command */
    if (H_FAILED(ret = _hantek_device_reset(hdl))) {
        goto done;
    }

    /* Read back the FPGA version */
    if (H_FAILED(ret = _hantek_get_fpga_version(hdl, &nhdev->fpga_version))) {
        DEBUG("Failed to get FPGA version, aborting.");
        goto done;
    }

    /* Send the wakeup sequence (seems to be all magical from static analysis) */
    if (H_FAILED(ret = _hantek_wake_device(hdl))) {
        DEBUG("Failure while trying to wake the device, aborting.");
        goto done;
    }

    /* Get the ID string */
    if (H_FAILED(ret = _hantek_get_id_string(hdl, nhdev->id_string, HT_MAX_INFO_STRING_LEN))) {
        goto done;
    }

#ifdef HT_DEBUG
    hexdump_dump_hex(nhdev->id_string, HT_MAX_INFO_STRING_LEN);
#endif

    /* TODO: parse out what we need from the ID string */

    /* Grab the calibration data for this device */
    if (H_FAILED(ret = _hantek_get_calibration_data(hdl, nhdev->cal_data, HT_CALIBRATION_INFO_ENTRIES))) {
        DEBUG("Failed to get device calibration data, aborting.");
        goto done;
    }

#ifdef HT_DEBUG
    printf("Calibration data:\n");
    hexdump_dump_hex(nhdev->cal_data, HT_CALIBRATION_INFO_ENTRIES * sizeof(uint16_t));
#endif

    *pdev = nhdev;
done:
    if (H_FAILED(ret)) {
        if (NULL != nhdev) {
            free(nhdev);
            nhdev = NULL;
        }

        if (NULL != hdl) {
            libusb_close(hdl);
        }

        if (NULL != dev) {
            libusb_unref_device(dev);
        }
    }
    return ret;
}

HRESULT hantek_close_device(struct hantek_device **pdev)
{
    HRESULT ret = H_OK;

    struct hantek_device *hdev = NULL;

    HASSERT_ARG(NULL != pdev);
    HASSERT_ARG(NULL != *pdev);

    hdev = *pdev;

    if (NULL != hdev->hdl) {
        libusb_close(hdev->hdl);
        hdev->hdl = NULL;
    }

    if (NULL != hdev->dev) {
        libusb_unref_device(hdev->dev);
        hdev->dev = NULL;
    }

    return ret;
}

HRESULT hantek_set_sampling_rate(struct hantek_device *dev, enum hantek_sample_times sample_spacing)
{
    HRESULT ret = H_OK;

    uint8_t message[6] = { HT_MSG_SET_TIME_DIVISION, 0x0 };
    uint32_t spacing = (uint32_t)sample_spacing - 1;
    size_t transferred = 0;

    HASSERT_ARG(NULL != dev);

    message[2] = spacing & 0xff;
    message[3] = (spacing >> 8) & 0xff;
    message[4] = (spacing >> 16) & 0xff;
    message[5] = (spacing >> 24) & 0xff;

    if (H_FAILED(ret = _hantek_bulk_cmd_out(dev->hdl, message, sizeof(message), &transferred))) {
        DEBUG("Failed to set sampling rate.");
        ret = H_ERR_BAD_SAMPLE_RATE;
        goto done;
    }

done:
    return ret;
}

static
uint8_t _hantek_channel_setup(enum hantek_volts_per_div volts_per_div, enum hantek_coupling coupling, bool bandwidth_limit)
{
    uint8_t state = 0;

    state |= (bandwidth_limit == true) << HT_CHAN_BW_LIMIT_SHIFT;
    state |= (volts_per_div < HT_VPD_1V) << HT_CHAN_LT1V_SHIFT;
    state |= (volts_per_div >= HT_VPD_1V) << HT_CHAN_GE1V_SHIFT;
    state |= (volts_per_div < HT_VPD_100MV) << HT_CHAN_LT100MV_SHIFT;
    state |= (volts_per_div >= HT_VPD_100MV) << HT_CHAN_GE100MV_SHIFT;
    state |= (coupling == HT_COUPLING_DC) << HT_CHAN_COUPLING_SHIFT;
    state |= 1 << 1;

    return state;
}

HRESULT hantek_configure_channel_frontend(struct hantek_device *dev, unsigned channel_num, enum hantek_volts_per_div volts_per_div, enum hantek_coupling coupling, bool bw_limit)
{
    HRESULT ret = H_OK;

    uint8_t message[8] = { HT_MSG_CONFIGURE_FRONTEND, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 };
    struct hantek_channel *this_chan = NULL;
    size_t transferred = 0;

    HASSERT_ARG(NULL != dev);
    HASSERT_ARG(channel_num < HT_MAX_CHANNELS);

    this_chan = &dev->channels[channel_num];

    this_chan->vpd = volts_per_div;
    this_chan->coupling = coupling;
    this_chan->bw_limit = bw_limit;

    for (size_t i = 0; i < HT_MAX_CHANNELS; i++) {
        struct hantek_channel *chan = &dev->channels[i];
        message[2 + i] = _hantek_channel_setup(chan->vpd, chan->coupling, chan->bw_limit);
    }

    message[6] = 0x1;

    if (H_FAILED(ret = _hantek_bulk_cmd_out(dev->hdl, message, sizeof(message), &transferred))) {
        DEBUG("Failed to send frontend configuration, aborting.");
        goto done;
    }

    if (sizeof(message) != transferred) {
        DEBUG("Failed to transfer frontend configuration, aborting.");
        ret = H_ERR_NOT_READY;
        goto done;
    }

    transferred = 0;

    /* This is what the SDK does */
    usleep(2000);

    message[7] = 0x1;

    if (H_FAILED(ret = _hantek_bulk_cmd_out(dev->hdl, message, sizeof(message), &transferred)))  {
        DEBUG("Failed to send second frontend configuration command, aborting.");
        goto done;
    }

    if (sizeof(message) != transferred) {
        DEBUG("Failed to send second stage of frontend configuration command, aborting.");
        ret = H_ERR_NOT_READY;
        goto done;
    }

    /* This is also what the SDK does */
    usleep(50000);
done:
    return ret;
}

static
HRESULT _hantek_set_trigger_mode(libusb_device_handle *hdl, enum hantek_trigger_mode mode, enum hantek_trigger_slope slope, enum hantek_coupling coupling)
{
    HRESULT ret = H_OK;

    uint8_t message[6] = { HT_MSG_CONFIGURE_TRIGGER, 0x0 };
    size_t transferred = 0;

    HASSERT_ARG(NULL != hdl);

    message[2] = (uint8_t)mode;
    message[3] = (uint8_t)slope;
    message[4] = (uint8_t)coupling;
    message[5] = 0x0;

    if (H_FAILED(ret = _hantek_bulk_cmd_out(hdl, message, sizeof(message), &transferred))) {
        DEBUG("Failed to send set trigger mode command, aborting.");
        goto done;
    }

done:
    return ret;
}

HRESULT hantek_configure_trigger(struct hantek_device *dev, unsigned channel_num, enum hantek_trigger_mode mode, enum hantek_trigger_slope slope, enum hantek_coupling coupling, uint8_t level)
{
    HRESULT ret = H_OK;

    HASSERT_ARG(NULL != dev);
    HASSERT_ARG(4 > channel_num);

    if (H_FAILED(ret = _hantek_set_trigger_mode(dev->hdl, mode, slope, coupling))) {
        goto done;
    }

    /* TODO: set trigger level */

done:
    return ret;
}

