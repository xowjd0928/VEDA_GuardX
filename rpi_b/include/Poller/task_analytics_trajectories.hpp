#pragma once

#include "Config/config.hpp"

#include <pqxx/pqxx>

/**
 * Polls CAP /analytics/trajectories and stores anonymous trajectory segments.
 *
 * @param cfg    Runtime configuration.
 * @param db     PostgreSQL connection.
 */
void pollAnalyticsTrajectories(const Config& cfg, pqxx::connection& db);
