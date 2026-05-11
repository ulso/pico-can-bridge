#include <zephyr/device.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/usb_ch9.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(usb_descriptors, CONFIG_LOG_DEFAULT_LEVEL);

#define USB_VID_ZEPHYR 0x2FE3
#define USB_PID_PICO_CAN_BRIDGE 0x0100

USBD_DEVICE_DEFINE(pico_can_bridge_usbd,
		   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   USB_VID_ZEPHYR, USB_PID_PICO_CAN_BRIDGE);

USBD_DESC_LANG_DEFINE(lang_desc);
USBD_DESC_MANUFACTURER_DEFINE(mfr_desc, "pico-can-bridge");
USBD_DESC_PRODUCT_DEFINE(product_desc, "Pico CAN Bridge CDC-NCM");
USBD_DESC_SERIAL_NUMBER_DEFINE(serial_desc);

USBD_DESC_CONFIG_DEFINE(fs_cfg_desc, "CDC-NCM");

USBD_CONFIGURATION_DEFINE(fs_config,
			  USB_SCD_SELF_POWERED,
			  100,
			  &fs_cfg_desc);

struct usbd_context *usb_device_init(void)
{
	int err;

	err = usbd_add_descriptor(&pico_can_bridge_usbd, &lang_desc);
	if (err) {
		LOG_ERR("language descriptor failed: %d", err);
		return NULL;
	}

	err = usbd_add_descriptor(&pico_can_bridge_usbd, &mfr_desc);
	if (err) {
		LOG_ERR("manufacturer descriptor failed: %d", err);
		return NULL;
	}

	err = usbd_add_descriptor(&pico_can_bridge_usbd, &product_desc);
	if (err) {
		LOG_ERR("product descriptor failed: %d", err);
		return NULL;
	}

	err = usbd_add_descriptor(&pico_can_bridge_usbd, &serial_desc);
	if (err) {
		LOG_ERR("serial descriptor failed: %d", err);
		return NULL;
	}

	err = usbd_add_configuration(&pico_can_bridge_usbd,
				     USBD_SPEED_FS,
				     &fs_config);
	if (err) {
		LOG_ERR("FS configuration failed: %d", err);
		return NULL;
	}

	err = usbd_register_all_classes(&pico_can_bridge_usbd,
					USBD_SPEED_FS,
					1,
					NULL);
	if (err) {
		LOG_ERR("class registration failed: %d", err);
		return NULL;
	}

	usbd_device_set_code_triple(&pico_can_bridge_usbd,
				    USBD_SPEED_FS,
				    USB_BCC_MISCELLANEOUS,
				    0x02,
				    0x01);
	usbd_self_powered(&pico_can_bridge_usbd, true);

	err = usbd_init(&pico_can_bridge_usbd);
	if (err) {
		LOG_ERR("USB device init failed: %d", err);
		return NULL;
	}

	return &pico_can_bridge_usbd;
}
