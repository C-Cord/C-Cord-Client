// SPDX-License-Identifier: AGPL-3.0-only WITH C-Cord-Extension-Exception
// Copyright (C) 2026 C-Cord contributors — see LICENSE and LICENSES/C-Cord-Extension-Exception.txt
#pragma once
// Private to the store module: expression data owned by struct ccord_store.
#include <ccord/store/expressions.h>
typedef struct ccord_expr_data ccord_expr_data;
void ccord_expr_data_free(ccord_expr_data *d);
ccord_expr_data **ccord_store_expr_slot_(ccord_store *s);               // defined in store.c
ccord_expr_data *const *ccord_store_expr_slot_const_(const ccord_store *s);
void ccord_store_expr_mutated_(ccord_store *s);
