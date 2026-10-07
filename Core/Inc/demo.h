/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * demo.h
 *
 *  Created on: Oct 14, 2024
 *      Author: GeorgeVigelette
 */

#ifndef INC_DEMO_H_
#define INC_DEMO_H_

#include "tx7332.h"
#include <stdbool.h>

extern unsigned int reg_values[][2];
extern unsigned int reg_1mhz_3p_values[][2];

void write_demo_registers(TX7332* pT);
bool verify_demo_registers(TX7332* pT);
void write_test_pattern_registers(TX7332* pT);
int verify_test_pattern_registers(TX7332* pT);

#endif /* INC_DEMO_H_ */
