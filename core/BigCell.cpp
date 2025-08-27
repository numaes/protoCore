/*
* BigCell.cpp
 *
 *  Created on: 2024
 *      Author: Gemini Code Assist
 *
 *  This file provides a non-inline definition for the BigCell constructor
 *  and destructor. Its purpose is to give the compiler a unique place
 *  to generate the v-table and RTTI information for the class,
 *  resolving "undefined reference to typeinfo" errors.
 */
#include "../headers/proto_internal.h"

namespace proto {
    BigCell::BigCell(ProtoContext* context) : Cell(context) {}
    BigCell::~BigCell() = default;
} // namespace proto