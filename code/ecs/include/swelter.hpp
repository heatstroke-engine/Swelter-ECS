#pragma once

/**
 * @file swelter.hpp
 * @brief Umbrella header of the Swelter ECS: include this one to use the whole library.
 *
 * Swelter is an archetype-based ECS: entities with the same set of components are
 * stored together in fixed-size chunks with SoA layout.
 */

#include <swelter/config.hpp>
#include <swelter/entity.hpp>
#include <swelter/registry.hpp>
#include <swelter/chunk.hpp>
#include <swelter/archetype.hpp>
#include <swelter/entity_table.hpp>
