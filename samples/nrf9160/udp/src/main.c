/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <stdio.h>
#include <modem/lte_lc.h>
#include <modem/sms.h>
#include <zephyr/net/socket.h>
#include <string.h>

#define UDP_IP_HEADER_SIZE 28
#define SMS_SEND_NUMBER "580011600030"

static struct pollfd client;
static int client_fd;
static struct sockaddr_storage host_addr;

K_SEM_DEFINE(lte_connected, 0, 1);

#if defined(CONFIG_NRF_MODEM_LIB)
static void lte_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if ((evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_HOME) &&
		     (evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_ROAMING)) {
			break;
		}

		printk("Network registration status: %s\n",
			evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ?
			"Connected - home network" : "Connected - roaming\n");
		k_sem_give(&lte_connected);
		break;
	case LTE_LC_EVT_PSM_UPDATE:
		printk("PSM parameter update: TAU: %d, Active time: %d\n",
			evt->psm_cfg.tau, evt->psm_cfg.active_time);
		break;
	case LTE_LC_EVT_EDRX_UPDATE: {
		char log_buf[60];
		ssize_t len;

		len = snprintf(log_buf, sizeof(log_buf),
			       "eDRX parameter update: eDRX: %f, PTW: %f\n",
			       evt->edrx_cfg.edrx, evt->edrx_cfg.ptw);
		if (len > 0) {
			printk("%s\n", log_buf);
		}
		break;
	}
	case LTE_LC_EVT_RRC_UPDATE:
		printk("RRC mode: %s\n",
			evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED ?
			"Connected" : "Idle\n");
		break;
	case LTE_LC_EVT_CELL_UPDATE:
		printk("LTE cell changed: Cell ID: %d, Tracking area: %d\n",
		       evt->cell.id, evt->cell.tac);
		break;
	default:
		break;
	}
}


static void modem_init(void)
{
	int err;

	if (IS_ENABLED(CONFIG_LTE_AUTO_INIT_AND_CONNECT)) {
		/* Do nothing, modem is already configured and LTE connected. */
	} else {
		err = lte_lc_init();
		if (err) {
			printk("Modem initialization failed, error: %d\n", err);
			return;
		}
	}
}

static void modem_connect(void)
{
	int err;

	if (IS_ENABLED(CONFIG_LTE_AUTO_INIT_AND_CONNECT)) {
		/* Do nothing, modem is already configured and LTE connected. */
	} else {
		err = lte_lc_connect_async(lte_handler);
		if (err) {
			printk("Connecting to LTE network failed, error: %d\n",
			       err);
			return;
		}
	}
}
#endif

static void server_disconnect(void)
{
	(void)close(client.fd);
}

static int server_init(void)
{
	struct sockaddr_in *server4 = ((struct sockaddr_in *)&host_addr);

	server4->sin_family = AF_INET;
	server4->sin_port = htons(CONFIG_UDP_SERVER_PORT);

	inet_pton(AF_INET, CONFIG_UDP_SERVER_ADDRESS_STATIC,
		  &server4->sin_addr);

	int err;

	client_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (client_fd < 0) {
		printk("Failed to create UDP socket: %d\n", errno);
		err = -errno;
		goto error;
	}

	err = connect(client_fd, (struct sockaddr *)&host_addr,
		      sizeof(struct sockaddr_in));
	if (err < 0) {
		printk("Connect failed : %d\n", errno);
		goto error;
	}

	client.events = POLLIN;

	return 0;

error:
	server_disconnect();

	return err;
}


static void sms_callback(struct sms_data *const data, void *context)
{
	if (data == NULL) {
		printk("%s with NULL data\n", __func__);
		return;
	}

	if (data->type == SMS_TYPE_STATUS_REPORT) {
		printk("SMS status report received\n");
		return;
	}

	if (data->type != SMS_TYPE_DELIVER) {
		printk("SMS protocol message with unknown type received\n");
		return;
	}

	int err = 0;
	static uint16_t concat_msg_len = 0;
	static uint8_t concat_buffer[CONFIG_NRF_COAP_MESSAGE_DATA_MAX_SIZE + 100];
	static uint8_t coap_data[CONFIG_NRF_COAP_MESSAGE_DATA_MAX_SIZE + 100];
	static uint16_t coap_data_size = 0;

	if (data->header.deliver.concatenated.present) {
		memcpy(&concat_buffer[concat_msg_len], data->payload, data->payload_len);
		concat_msg_len += data->payload_len;
		if (data->header.deliver.concatenated.seq_number ==
			data->header.deliver.concatenated.total_msgs) {

			coap_data_size = concat_msg_len - 6;
			memcpy(coap_data, &concat_buffer[6], coap_data_size);

			err = send(client_fd, coap_data, coap_data_size, 0);

			if (err < 0) {
				printk("Failed to transmit UDP concat packet, %d\n", errno);
				concat_msg_len = 0;
				return;
			}

			printk("Sent UDP package: %d/%d\n", err, coap_data_size);
			concat_msg_len = 0;
		}
	} else {
		coap_data_size = data->payload_len - 6;
		memcpy(coap_data, &data->payload[6], coap_data_size);

		err = send(client_fd, coap_data, coap_data_size, 0);

		if (err < 0) {
			printk("Failed to transmit UDP packet, %d\n", errno);
			return;
		}
		printk("Sent UDP package: %d/%d\n", err, coap_data_size);

	}
}

static int sms_init(void)
{
	int handle = sms_register_listener(sms_callback, NULL);
	if (handle) {
		printk("sms_register_listener returned err: %d\n", handle);
		return handle;
	}
	printk("Registered as sms listener\n");
	return 0;
}

static int poll_in_handler(void)
{
	static uint8_t coap_buffer[CONFIG_NRF_COAP_MESSAGE_DATA_MAX_SIZE + 100];
	uint16_t len = recv(client.fd, coap_buffer, sizeof(coap_buffer), 0);

	if (len < 0) {
		printk("Received failed\n");
		return -ENOMSG;
	}

	printk("Received CoAP package from server\n");

	// 01 - Push ID
	// 06 - PDU type: push
	// 03 - Header length
	// C4 - Content type code: application/vnd.syncml.notification
	// AF - WAP app ID
	// 9A - URN ID - x-wap-application:lwm2m.dm
	uint8_t wsp[] =  {
		0x01, 0x06, 0x03, 0xC4, 0xAF, 0x9A
	};

	uint8_t sms_data[len + sizeof(wsp)];
	uint16_t sms_data_len = sizeof(sms_data);

	memcpy(sms_data, wsp, sizeof(wsp));
	memcpy(sms_data + 6, coap_buffer, len);

	// 06 - UDHL: length
	// 05 - IEID: 16-bit application port addressing
	// 04 - Information Element Data length
	// 0B84 - Destination port: 2948
	// 23F0 - Originating port
	char udh_str[] = "0605040B8423F0";

	return sms_send_data(SMS_SEND_NUMBER, sms_data, sms_data_len, udh_str);
}

static bool poll_succeed(void)
{
	printk("Starting poll\n");
	int ret = poll(&client, 1, -1);
	printk("Poll finished\n");

	if (ret == 0) {
		printk("Timeout\n");
		return true;
	} else if (ret < 0) {
		printk("Error in poll\n");
	}

	if (client.revents & POLLIN) {
		ret = poll_in_handler();
		if (ret < 0) {
			printk("Failed to send sms\n");
		}
	}
	printk("End of poll_succeed\n");
	return true;
}

void main(void)
{
	int err;

	printk("Gateway sample has started\n");

#if defined(CONFIG_NRF_MODEM_LIB)
	modem_init();
	modem_connect();

	k_sem_take(&lte_connected, K_FOREVER);
#endif

	err = server_init();
	if (err) {
		printk("Not able to initialize UDP server connection\n");
		goto err;
	}

	err = sms_init();
	if (err) {
		printk("Not able to initialize SMS listener");
		goto err;
	}

	while (poll_succeed()) {

	}
	return;
err:
	while (true) {}
}
