/*
 * ESP32-P4 100 Mbps Ethernet: EMAC (RMII) + IP101GA PHY
 *
 * The guest owns the NIC, exactly like the WiFi path in wifi.c: the ESP's own
 * TCP/IP stack is bypassed instead of being bridged.
 *
 *   guest TX: guest -> ne2000 -> esp32_send_packet() -> esp_eth_transmit()
 *   guest RX: EMAC -> eth_input() -> ne2000_l2_input() -> ne2000_receive()
 *
 * No ESP-IDF patch is needed here: esp_eth_update_input_path() / the
 * esp_eth_config_t::stack_input field is public API.  The lwIP wlanif hook in
 * src/esp/esp-idf.patch is only required for the WiFi back end.
 */
#ifdef USE_ETH

#include <stdlib.h>
#include <stdatomic.h>

#include "esp_log.h"
#include "esp_event.h"
#include "esp_eth.h"

#include "common.h"
#include "../../ne2000.h"

static const char *TAG = "eth";

/* Defined in wifi.c; the guest's only route to the outside world. */
extern void (*_Atomic esp32_send_packet)(uint8_t *buf, int size);

static esp_eth_handle_t s_eth_handle;

static void eth_send_packet(uint8_t *buf, int size)
{
	if (s_eth_handle == NULL)
		return;
	esp_eth_transmit(s_eth_handle, buf, size);
}

/* Frames arrive in a heap buffer owned by the EMAC driver.  ne2000_receive()
 * copies the part it needs out of it, so the buffer is ours to release. */
static esp_err_t eth_input(esp_eth_handle_t hdl, uint8_t *buffer,
			   uint32_t length, void *priv)
{
	(void)hdl;
	(void)priv;

	if (buffer) {
		ne2000_l2_input(buffer, length);
		free(buffer);
	}
	return ESP_OK;
}

static int eth_get_mac(uint8_t *mac)
{
	if (s_eth_handle == NULL)
		return -1;
	return esp_eth_ioctl(s_eth_handle, ETH_CMD_G_MAC_ADDR, mac) == ESP_OK ? 0 : -1;
}

void eth_main(void)
{
	eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
#ifdef ETH_USE_IDF_DEFAULT
	/* ESP32-S31: the IDF default is already the board's RGMII wiring
	 * (mdc=5, mdio=6, rxc=14, txc=13, tx_ctl=12, txd0..3=8..11,
	 *  rx_ctl=15, rxd0..3=19/18/17/16).  Nothing to override here. */
#else
	emac_config.smi_gpio.mdc_num = ETH_MDC_GPIO;
	emac_config.smi_gpio.mdio_num = ETH_MDIO_GPIO;
	emac_config.interface = EMAC_DATA_INTERFACE_RMII;
#ifdef ETH_RMII_CLK_INPUT
	/* The PHY sources the 50 MHz RMII REF_CLK into the P4's 50M_CLKI pad. */
	emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
	emac_config.clock_config.rmii.clock_gpio = ETH_RMII_CLK_GPIO;
#else
	/* The P4 sources the 50 MHz RMII REF_CLK and drives the PHY's 50M_CLKI. */
	emac_config.clock_config.rmii.clock_mode = EMAC_CLK_OUT;
	emac_config.clock_config.rmii.clock_gpio = ETH_RMII_CLK_GPIO;
#endif
	emac_config.emac_dataif_gpio.rmii.tx_en_num = ETH_TX_EN_GPIO;
	emac_config.emac_dataif_gpio.rmii.txd0_num = ETH_TXD0_GPIO;
	emac_config.emac_dataif_gpio.rmii.txd1_num = ETH_TXD1_GPIO;
	emac_config.emac_dataif_gpio.rmii.crs_dv_num = ETH_CRS_DV_GPIO;
	emac_config.emac_dataif_gpio.rmii.rxd0_num = ETH_RXD0_GPIO;
	emac_config.emac_dataif_gpio.rmii.rxd1_num = ETH_RXD1_GPIO;
#endif

	eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
	/* Pin the EMAC RX task to the core this install runs on.  eth_main() is
	 * invoked from a Core 1 task (see eth_init_task in esp_main.c), so the
	 * RX task lands on Core 1 and never steals cycles from the x86 emulator
	 * that owns Core 0. */
	mac_config.flags |= ETH_MAC_FLAG_PIN_TO_CORE;
	esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);

	eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
	phy_config.phy_addr = ETH_PHY_ADDR;
	/* The IP101GA has no dedicated reset line on this board. */
	phy_config.reset_gpio_num = -1;
	/* The IP101GA is a plain IEEE 802.3 PHY, so the generic driver covers it.
	 * The dedicated esp_eth_phy_new_ip101() was removed in ESP-IDF v6.0. */
	esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);

	/* esp_eth_start() posts ETH_EVENT, which fails without a default loop. */
	esp_err_t err = esp_event_loop_create_default();
	if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
		ESP_ERROR_CHECK(err);

	esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
	eth_config.stack_input = eth_input;
	ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &s_eth_handle));

	/* The guest's MAC comes from its own adapter, which may differ from the
	 * EMAC's efuse MAC; accept everything and let the guest filter. */
	bool promiscuous = true;
	ESP_ERROR_CHECK(esp_eth_ioctl(s_eth_handle, ETH_CMD_S_PROMISCUOUS, &promiscuous));

	uint8_t macaddr[6];
	if (eth_get_mac(macaddr) == 0) {
#ifdef ETH_USE_IDF_DEFAULT
		ESP_LOGI(TAG, "RGMII MAC %02x:%02x:%02x:%02x:%02x:%02x (IDF default pins)",
			 macaddr[0], macaddr[1], macaddr[2], macaddr[3], macaddr[4], macaddr[5]);
#else
		ESP_LOGI(TAG, "RMII MAC %02x:%02x:%02x:%02x:%02x:%02x, mdc=%d mdio=%d refclk=%d",
			 macaddr[0], macaddr[1], macaddr[2], macaddr[3], macaddr[4], macaddr[5],
			 ETH_MDC_GPIO, ETH_MDIO_GPIO, ETH_RMII_CLK_GPIO);
#endif
	}

	ESP_ERROR_CHECK(esp_eth_start(s_eth_handle));

	atomic_store_explicit(&esp32_send_packet, eth_send_packet,
			      memory_order_relaxed);
}

#endif /* USE_ETH */