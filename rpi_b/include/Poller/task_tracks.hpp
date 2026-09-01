#pragma once

#include "Config/config.hpp"
#include <pqxx/pqxx>

/**
 * Polls CAP /tracks snapshot and stores global tracking data.
 *
 * @param cfg    Runtime configuration.
 * @param db     PostgreSQL connection.
 */
void pollTracks(const Config& cfg, pqxx::connection& db);