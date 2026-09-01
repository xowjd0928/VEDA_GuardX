#include "Database/db.hpp"

int lookupZoneId(pqxx::connection& db, int camera_id, int channel) {
  pqxx::work tx(db);
  pqxx::result r = tx.exec(
      "SELECT zone_id FROM zones WHERE camera_id=$1 AND channel=$2 LIMIT 1",
      pqxx::params{camera_id, channel});
  tx.commit();
  if (r.empty()) return -1;
  return r[0][0].as<int>();
}