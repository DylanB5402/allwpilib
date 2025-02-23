// Copyright (c) FIRST and other WPILib contributors.
// Open Source Software; you can modify and/or share it under the terms of
// the WPILib BSD license file in the root directory of this project.

#include "frc/PneumaticHub.h"

#include <hal/REVPH.h>
#include <wpi/NullDeleter.h>
#include <wpi/StackTrace.h>

#include "frc/Compressor.h"
#include "frc/DoubleSolenoid.h"
#include "frc/Errors.h"
#include "frc/SensorUtil.h"
#include "frc/Solenoid.h"

using namespace frc;

wpi::mutex PneumaticHub::m_handleLock;
std::unique_ptr<wpi::DenseMap<int, std::weak_ptr<PneumaticHub::DataStore>>>
    PneumaticHub::m_handleMap = nullptr;

// Always called under lock, so we can avoid the double lock from the magic
// static
std::weak_ptr<PneumaticHub::DataStore>& PneumaticHub::GetDataStore(int module) {
  if (!m_handleMap) {
    m_handleMap = std::make_unique<
        wpi::DenseMap<int, std::weak_ptr<PneumaticHub::DataStore>>>();
  }
  return (*m_handleMap)[module];
}

class PneumaticHub::DataStore {
 public:
  explicit DataStore(int module, const char* stackTrace) {
    int32_t status = 0;
    HAL_REVPHHandle handle = HAL_InitializeREVPH(module, stackTrace, &status);
    FRC_CheckErrorStatus(status, "Module {}", module);
    m_moduleObject = PneumaticHub{handle, module};
    m_moduleObject.m_dataStore =
        std::shared_ptr<DataStore>{this, wpi::NullDeleter<DataStore>()};
  }

  ~DataStore() noexcept { HAL_FreeREVPH(m_moduleObject.m_handle); }

  DataStore(DataStore&&) = delete;
  DataStore& operator=(DataStore&&) = delete;

 private:
  friend class PneumaticHub;
  uint32_t m_reservedMask{0};
  bool m_compressorReserved{false};
  wpi::mutex m_reservedLock;
  PneumaticHub m_moduleObject{HAL_kInvalidHandle, 0};
  std::array<units::second_t, 16> m_oneShotDurMs{0_s};
};

PneumaticHub::PneumaticHub()
    : PneumaticHub{SensorUtil::GetDefaultREVPHModule()} {}

PneumaticHub::PneumaticHub(int module) {
  std::string stackTrace = wpi::GetStackTrace(1);
  std::scoped_lock lock(m_handleLock);
  auto& res = GetDataStore(module);
  m_dataStore = res.lock();
  if (!m_dataStore) {
    m_dataStore = std::make_shared<DataStore>(module, stackTrace.c_str());
    res = m_dataStore;
  }
  m_handle = m_dataStore->m_moduleObject.m_handle;
  m_module = module;
}

PneumaticHub::PneumaticHub(HAL_REVPHHandle handle, int module)
    : m_handle{handle}, m_module{module} {}

bool PneumaticHub::GetCompressor() const {
  int32_t status = 0;
  auto result = HAL_GetREVPHCompressor(m_handle, &status);
  FRC_ReportError(status, "Module {}", m_module);
  return result;
}

void PneumaticHub::DisableCompressor() {
  int32_t status = 0;
  HAL_SetREVPHClosedLoopControlDisabled(m_handle, &status);
  FRC_ReportError(status, "Module {}", m_module);
}

void PneumaticHub::EnableCompressorDigital() {
  int32_t status = 0;
  HAL_SetREVPHClosedLoopControlDigital(m_handle, &status);
  FRC_ReportError(status, "Module {}", m_module);
}

void PneumaticHub::EnableCompressorAnalog(units::volt_t minAnalogVoltage,
                                          units::volt_t maxAnalogVoltage) {
  int32_t status = 0;
  HAL_SetREVPHClosedLoopControlAnalog(m_handle, minAnalogVoltage.value(),
                                      maxAnalogVoltage.value(), &status);
  FRC_ReportError(status, "Module {}", m_module);
}

void PneumaticHub::EnableCompressorHybrid(units::volt_t minAnalogVoltage,
                                          units::volt_t maxAnalogVoltage) {
  int32_t status = 0;
  HAL_SetREVPHClosedLoopControlHybrid(m_handle, minAnalogVoltage.value(),
                                      maxAnalogVoltage.value(), &status);
  FRC_ReportError(status, "Module {}", m_module);
}

CompressorConfigType PneumaticHub::GetCompressorConfigType() const {
  int32_t status = 0;
  auto result = HAL_GetREVPHCompressorConfig(m_handle, &status);
  FRC_ReportError(status, "Module {}", m_module);
  return static_cast<CompressorConfigType>(result);
}

bool PneumaticHub::GetPressureSwitch() const {
  int32_t status = 0;
  auto result = HAL_GetREVPHPressureSwitch(m_handle, &status);
  FRC_ReportError(status, "Module {}", m_module);
  return result;
}

units::ampere_t PneumaticHub::GetCompressorCurrent() const {
  int32_t status = 0;
  auto result = HAL_GetREVPHCompressorCurrent(m_handle, &status);
  FRC_ReportError(status, "Module {}", m_module);
  return units::ampere_t{result};
}

void PneumaticHub::SetSolenoids(int mask, int values) {
  int32_t status = 0;
  HAL_SetREVPHSolenoids(m_handle, mask, values, &status);
  FRC_ReportError(status, "Module {}", m_module);
}

int PneumaticHub::GetSolenoids() const {
  int32_t status = 0;
  auto result = HAL_GetREVPHSolenoids(m_handle, &status);
  FRC_ReportError(status, "Module {}", m_module);
  return result;
}

int PneumaticHub::GetModuleNumber() const {
  return m_module;
}

int PneumaticHub::GetSolenoidDisabledList() const {
  int32_t status = 0;
  HAL_REVPHStickyFaults faults;
  std::memset(&faults, 0, sizeof(faults));
  HAL_GetREVPHStickyFaults(m_handle, &faults, &status);
  FRC_ReportError(status, "Module {}", m_module);
  uint32_t intFaults = 0;
  static_assert(sizeof(faults) == sizeof(intFaults));
  std::memcpy(&intFaults, &faults, sizeof(faults));
  return intFaults & 0xFFFF;
}

void PneumaticHub::FireOneShot(int index) {
  int32_t status = 0;
  HAL_FireREVPHOneShot(m_handle, index,
                       m_dataStore->m_oneShotDurMs[index].value(), &status);
  FRC_ReportError(status, "Module {}", m_module);
}

void PneumaticHub::SetOneShotDuration(int index, units::second_t duration) {
  m_dataStore->m_oneShotDurMs[index] = duration;
}

bool PneumaticHub::CheckSolenoidChannel(int channel) const {
  return HAL_CheckREVPHSolenoidChannel(channel);
}

int PneumaticHub::CheckAndReserveSolenoids(int mask) {
  std::scoped_lock lock{m_dataStore->m_reservedLock};
  uint32_t uMask = static_cast<uint32_t>(mask);
  if ((m_dataStore->m_reservedMask & uMask) != 0) {
    return m_dataStore->m_reservedMask & uMask;
  }
  m_dataStore->m_reservedMask |= uMask;
  return 0;
}

void PneumaticHub::UnreserveSolenoids(int mask) {
  std::scoped_lock lock{m_dataStore->m_reservedLock};
  m_dataStore->m_reservedMask &= ~(static_cast<uint32_t>(mask));
}

bool PneumaticHub::ReserveCompressor() {
  std::scoped_lock lock{m_dataStore->m_reservedLock};
  if (m_dataStore->m_compressorReserved) {
    return false;
  }
  m_dataStore->m_compressorReserved = true;
  return true;
}

void PneumaticHub::UnreserveCompressor() {
  std::scoped_lock lock{m_dataStore->m_reservedLock};
  m_dataStore->m_compressorReserved = false;
}

PneumaticHub::Version PneumaticHub::GetVersion() const {
  int32_t status = 0;
  HAL_REVPHVersion halVersions;
  std::memset(&halVersions, 0, sizeof(halVersions));
  HAL_GetREVPHVersion(m_handle, &halVersions, &status);
  FRC_ReportError(status, "Module {}", m_module);
  PneumaticHub::Version versions;
  static_assert(sizeof(halVersions) == sizeof(versions));
  static_assert(std::is_standard_layout_v<decltype(versions)>);
  static_assert(std::is_trivial_v<decltype(versions)>);
  std::memcpy(&versions, &halVersions, sizeof(versions));
  return versions;
}

PneumaticHub::Faults PneumaticHub::GetFaults() const {
  int32_t status = 0;
  HAL_REVPHFaults halFaults;
  std::memset(&halFaults, 0, sizeof(halFaults));
  HAL_GetREVPHFaults(m_handle, &halFaults, &status);
  FRC_ReportError(status, "Module {}", m_module);
  PneumaticHub::Faults faults;
  static_assert(sizeof(halFaults) == sizeof(faults));
  static_assert(std::is_standard_layout_v<decltype(faults)>);
  static_assert(std::is_trivial_v<decltype(faults)>);
  std::memcpy(&faults, &halFaults, sizeof(faults));
  return faults;
}

PneumaticHub::StickyFaults PneumaticHub::GetStickyFaults() const {
  int32_t status = 0;
  HAL_REVPHStickyFaults halStickyFaults;
  std::memset(&halStickyFaults, 0, sizeof(halStickyFaults));
  HAL_GetREVPHStickyFaults(m_handle, &halStickyFaults, &status);
  FRC_ReportError(status, "Module {}", m_module);
  PneumaticHub::StickyFaults stickyFaults;
  static_assert(sizeof(halStickyFaults) == sizeof(stickyFaults));
  static_assert(std::is_standard_layout_v<decltype(stickyFaults)>);
  static_assert(std::is_trivial_v<decltype(stickyFaults)>);
  std::memcpy(&stickyFaults, &halStickyFaults, sizeof(stickyFaults));
  return stickyFaults;
}

void PneumaticHub::ClearStickyFaults() {
  int32_t status = 0;
  HAL_ClearREVPHStickyFaults(m_handle, &status);
  FRC_ReportError(status, "Module {}", m_module);
}

units::volt_t PneumaticHub::GetInputVoltage() const {
  int32_t status = 0;
  auto voltage = HAL_GetREVPHVoltage(m_handle, &status);
  FRC_ReportError(status, "Module {}", m_module);
  return units::volt_t{voltage};
}

units::volt_t PneumaticHub::Get5VRegulatedVoltage() const {
  int32_t status = 0;
  auto voltage = HAL_GetREVPH5VVoltage(m_handle, &status);
  FRC_ReportError(status, "Module {}", m_module);
  return units::volt_t{voltage};
}

units::ampere_t PneumaticHub::GetSolenoidsTotalCurrent() const {
  int32_t status = 0;
  auto current = HAL_GetREVPHSolenoidCurrent(m_handle, &status);
  FRC_ReportError(status, "Module {}", m_module);
  return units::ampere_t{current};
}

units::volt_t PneumaticHub::GetSolenoidsVoltage() const {
  int32_t status = 0;
  auto voltage = HAL_GetREVPHSolenoidVoltage(m_handle, &status);
  FRC_ReportError(status, "Module {}", m_module);
  return units::volt_t{voltage};
}

units::volt_t PneumaticHub::GetAnalogVoltage(int channel) const {
  int32_t status = 0;
  auto voltage = HAL_GetREVPHAnalogVoltage(m_handle, channel, &status);
  FRC_ReportError(status, "Module {}", m_module);
  return units::volt_t{voltage};
}

Solenoid PneumaticHub::MakeSolenoid(int channel) {
  return Solenoid{m_module, PneumaticsModuleType::REVPH, channel};
}

DoubleSolenoid PneumaticHub::MakeDoubleSolenoid(int forwardChannel,
                                                int reverseChannel) {
  return DoubleSolenoid{m_module, PneumaticsModuleType::REVPH, forwardChannel,
                        reverseChannel};
}

Compressor PneumaticHub::MakeCompressor() {
  return Compressor{m_module, PneumaticsModuleType::REVPH};
}

std::shared_ptr<PneumaticsBase> PneumaticHub::GetForModule(int module) {
  std::string stackTrace = wpi::GetStackTrace(1);
  std::scoped_lock lock(m_handleLock);
  auto& res = GetDataStore(module);
  std::shared_ptr<DataStore> dataStore = res.lock();
  if (!dataStore) {
    dataStore = std::make_shared<DataStore>(module, stackTrace.c_str());
    res = dataStore;
  }

  return std::shared_ptr<PneumaticsBase>{dataStore, &dataStore->m_moduleObject};
}
