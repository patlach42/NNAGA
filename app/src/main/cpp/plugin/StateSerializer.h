/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of Guitar RackCraft.
 * Guitar RackCraft is free software under the GNU General Public License v3.
 */
#ifndef GUITARRACKCRAFT_STATE_SERIALIZER_H
#define GUITARRACKCRAFT_STATE_SERIALIZER_H

#include "RackGraph.h"
#include <string>

namespace guitarrackcraft {

/** Serialize the complete rack graph in version 2 format. */
std::string serializeRackStateToJson(const RackGraph::State& state);

} // namespace guitarrackcraft

#endif // GUITARRACKCRAFT_STATE_SERIALIZER_H
