#pragma once
// task_detections — GET /detections?since=<cursor> → detections INSERT (증분)
#include <pqxx/pqxx>
#include "Config/config.hpp"
#include "Storage/state.hpp"

void pollDetections(const Config& cfg, pqxx::connection& db, State& st);

