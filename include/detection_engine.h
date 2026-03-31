#ifndef DETECTION_ENGINE_H
#define DETECTION_ENGINE_H

#include "detection_types.h"
#include "rf_scanner.h"
#include "gps_manager.h"
#include "gnss_integrity.h"

void detectionEngineInit();

// Feed CAD and FSK detection counts before calling detectionEngineUpdate().
// Called from loRaScanTask after CAD/FSK scans complete.
void detectionEngineSetCadFsk(int cadCount, int fskCount, int strongPendingCad = 0);

// Run detection pipeline: peak extraction → freq matching → persistence → threat FSM.
// Called from loRaScanTask after each sweep. NOT thread-safe — single caller only.
ThreatLevel detectionEngineUpdate(const ScanResult& scan, const GpsData& gps,
                                  const IntegrityStatus& integrity,
                                  const ScanResult24* scan24 = nullptr);

#endif // DETECTION_ENGINE_H
