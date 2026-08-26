/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of NNAGA.
 * NNAGA is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#ifndef GUITARRACKCRAFT_RACK_STATE_CODEC_H
#define GUITARRACKCRAFT_RACK_STATE_CODEC_H
#include "RackGraph.h"
#include <string>
#include <vector>
namespace guitarrackcraft {
class RackStateCodec {
public:
    static constexpr uint32_t kMaxBlobBytes = 16u * 1024u * 1024u;
    static std::vector<uint8_t> encode(const RackGraph::State& state, std::string* error = nullptr);
    static bool decode(const uint8_t* data, size_t size, RackGraph::State& state, std::string& error);
};
}
#endif
