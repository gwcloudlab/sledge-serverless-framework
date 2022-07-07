#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct module_config {
	char    *name;
	char    *path;
	uint16_t port;
	uint8_t  admissions_percentile;
	uint32_t expected_execution_us;
	uint32_t relative_deadline_us;
	uint32_t replenishment_period_us;
	uint32_t max_budget_us;
	uint8_t  reservation_percentile;
	uint32_t http_req_size;
	uint32_t http_resp_size;
	char    *http_resp_content_type;
};

static inline void
module_config_deinit(struct module_config *config)
{
	free(config->name);
	free(config->path);
	free(config->http_resp_content_type);
}

static inline void
print_module_config(struct module_config *config)
{
	printf("Name: %s\n", config->name);
	printf("Path: %s\n", config->path);
	printf("Port: %u\n", config->port);
	printf("admissions_percentile: %u\n", config->admissions_percentile);
	printf("expected_execution_us: %u\n", config->expected_execution_us);
	printf("relative_deadline_us: %u\n", config->relative_deadline_us);
	printf("replenishment_period_us: %u\n", config->replenishment_period_us);
	printf("max_budget_us: %u\n", config->max_budget_us);
	printf("reservation_percentile: %u\n", config->reservation_percentile);
	printf("http_req_size: %u\n", config->http_req_size);
	printf("http_resp_size: %u\n", config->http_resp_size);
	printf("http_resp_content_type: %s\n", config->http_resp_content_type);
}
