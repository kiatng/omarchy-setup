#pragma once
#include <QString>
struct Inputs {
  bool requested = false, ac = true, lid = false, held = false;
  double battery = -1;
  int reserve = 15;
};
struct Decision {
  QString state, reason;
  bool inhibit = false, pending = false;
};
inline Decision decide(const Inputs &i) {
  if (i.requested && !i.ac && i.battery >= 0 && i.battery <= i.reserve)
    return {"sleep-pending", "battery-reserve", true, true};
  if (i.requested && !i.ac && i.battery < 0)
    return {"error", "battery-unavailable", false, false};
  if (i.requested) {
    if (!i.ac && i.battery <= i.reserve + 10)
      return {"battery-warning", "battery-low", true, false};
    return {"keeping-awake", i.ac ? "external-power" : "battery", true, false};
  }
  if (i.held && i.lid)
    return {"sleep-pending", "holds-released", true, true};
  return {"normal", "no-holds", false, false};
}
