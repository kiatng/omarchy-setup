#include "backend.h"
#include "service.h"
Backend::Backend() {
  connect(&timer, &QTimer::timeout, this, &Backend::refresh);
  timer.start(2000);
  refresh();
}
void Backend::refresh() {
  auto r = callService({{"command", "status"}});
  if (r.value("ok").toBool())
    data = r.toVariantMap();
  message = r.value("error").toString();
  emit changed();
}
void Backend::action(QString command, int value) {
  auto r = callService({{"command", command}, {"value", value}});
  if (r.value("ok").toBool())
    data = r.toVariantMap();
  message = r.value("error").toString();
  emit changed();
}
