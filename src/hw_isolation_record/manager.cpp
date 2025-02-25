// SPDX-License-Identifier: Apache-2.0

#include "config.h"

#include "hw_isolation_record/manager.hpp"

#include "common/common_types.hpp"
#include "common/utils.hpp"

#include <fmt/format.h>

#include <cereal/archives/binary.hpp>
#include <phosphor-logging/elog-errors.hpp>
#include <xyz/openbmc_project/State/Chassis/server.hpp>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <ranges>
#include <sstream>

// Associate Manager Class with version number
constexpr uint32_t Cereal_ManagerClassVersion = 1;
CEREAL_CLASS_VERSION(hw_isolation::record::Manager, Cereal_ManagerClassVersion);

namespace hw_isolation
{
namespace record
{

using namespace phosphor::logging;
namespace fs = std::filesystem;

constexpr auto HW_ISOLATION_ENTRY_MGR_PERSIST_PATH =
    "/var/lib/op-hw-isolation/persistdata/record_mgr/{}";

Manager::Manager(sdbusplus::bus::bus& bus, const std::string& objPath,
                 const sdeventplus::Event& eventLoop) :
    type::ServerObject<CreateInterface, OP_CreateInterface, DeleteAllInterface>(
        bus, objPath.c_str()),
    _bus(bus), _eventLoop(eventLoop), _isolatableHWs(bus),
    _guardFileWatch(
        eventLoop.get(), IN_NONBLOCK, IN_CLOSE_WRITE, EPOLLIN,
        openpower_guard::getGuardFilePath(),
        std::bind(std::mem_fn(&hw_isolation::record::Manager::
                                  processHardwareIsolationRecordFile),
                  this))
{
    fs::create_directories(
        fs::path(HW_ISOLATION_ENTRY_PERSIST_PATH).parent_path());

    deserialize();
}

void Manager::serialize()
{
    fs::path path{
        fmt::format(HW_ISOLATION_ENTRY_MGR_PERSIST_PATH, "eco_cores")};

    if (_persistedEcoCores.empty())
    {
        fs::remove(path);
        return;
    }

    fs::create_directories(path.parent_path());
    try
    {
        std::ofstream os(path.c_str(), std::ios::binary);
        cereal::BinaryOutputArchive oarchive(os);
        oarchive(*this);
    }
    catch (const cereal::Exception& e)
    {
        log<level::ERR>(fmt::format("Exception: [{}] during serialize the "
                                    "eco cores physical path into {}",
                                    e.what(), path.string())
                            .c_str());
        fs::remove(path);
    }
}

bool Manager::deserialize()
{
    fs::path path{
        fmt::format(HW_ISOLATION_ENTRY_MGR_PERSIST_PATH, "eco_cores")};
    try
    {
        if (fs::exists(path))
        {
            std::ifstream is(path.c_str(), std::ios::in | std::ios::binary);
            cereal::BinaryInputArchive iarchive(is);
            iarchive(*this);
            return true;
        }
        return false;
    }
    catch (const cereal::Exception& e)
    {
        log<level::ERR>(fmt::format("Exception: [{}] during deserialize the "
                                    "eco cores physical path into {}",
                                    e.what(), path.string())
                            .c_str());
        fs::remove(path);
        return false;
    }
}

void Manager::updateEcoCoresList(
    const bool ecoCore, const devtree::DevTreePhysPath& coreDevTreePhysPath)
{
    if (ecoCore)
    {
        _persistedEcoCores.emplace(coreDevTreePhysPath);
    }
    else
    {
        _persistedEcoCores.erase(coreDevTreePhysPath);
    }
    serialize();
}

std::optional<uint32_t>
    Manager::getEID(const sdbusplus::message::object_path& bmcErrorLog) const
{
    try
    {
        uint32_t eid;

        auto dbusServiceName = utils::getDBusServiceName(
            _bus, type::LoggingObjectPath, type::LoggingInterface);

        auto method = _bus.new_method_call(
            dbusServiceName.c_str(), type::LoggingObjectPath,
            type::LoggingInterface, "GetPELIdFromBMCLogId");

        method.append(static_cast<uint32_t>(std::stoi(bmcErrorLog.filename())));
        auto resp = _bus.call(method);

        resp.read(eid);
        return eid;
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        log<level::ERR>(fmt::format("Exception [{}] to get EID (aka PEL ID) "
                                    "for object [{}]",
                                    e.what(), bmcErrorLog.str)
                            .c_str());
    }
    return std::nullopt;
}

std::optional<sdbusplus::message::object_path> Manager::createEntry(
    const entry::EntryRecordId& recordId, const entry::EntryResolved& resolved,
    const entry::EntrySeverity& severity, const std::string& isolatedHardware,
    const std::string& bmcErrorLog, const bool deleteRecord,
    const openpower_guard::EntityPath& entityPath)
{
    try
    {
        auto entryObjPath =
            fs::path(HW_ISOLATION_ENTRY_OBJPATH) / std::to_string(recordId);

        // Add association for isolated hardware inventory path
        // Note: Association forward and reverse type are defined as per
        // hardware isolation design document (aka guard) and hardware isolation
        // entry dbus interface document for hardware and error object path
        type::AsscDefFwdType isolateHwFwdType("isolated_hw");
        type::AsscDefRevType isolatedHwRevType("isolated_hw_entry");
        type::AssociationDef associationDeftoHw;
        associationDeftoHw.push_back(std::make_tuple(
            isolateHwFwdType, isolatedHwRevType, isolatedHardware));

        // Add errog log as Association if given
        if (!bmcErrorLog.empty())
        {
            type::AsscDefFwdType bmcErrorLogFwdType("isolated_hw_errorlog");
            type::AsscDefRevType bmcErrorLogRevType("isolated_hw_entry");
            associationDeftoHw.push_back(std::make_tuple(
                bmcErrorLogFwdType, bmcErrorLogRevType, bmcErrorLog));
        }

        _isolatedHardwares.insert(std::make_pair(
            recordId, std::make_unique<entry::Entry>(
                          _bus, entryObjPath, *this, recordId, severity,
                          resolved, associationDeftoHw, entityPath)));

        utils::setEnabledProperty(_bus, isolatedHardware, resolved);

        // Update the last entry id by using the created entry id.
        return entryObjPath.string();
    }
    catch (const std::exception& e)
    {
        log<level::ERR>(
            fmt::format("Exception [{}], so failed to create entry", e.what())
                .c_str());

        if (deleteRecord)
        {
            openpower_guard::clear(recordId);
        }
    }
    return std::nullopt;
}

std::pair<bool, sdbusplus::message::object_path> Manager::updateEntry(
    const entry::EntryRecordId& recordId, const entry::EntrySeverity& severity,
    const std::string& isolatedHwDbusObjPath, const std::string& bmcErrorLog,
    const openpower_guard::EntityPath& entityPath)
{
    auto isolatedHwIt = std::find_if(
        _isolatedHardwares.begin(), _isolatedHardwares.end(),
        [recordId, entityPath](const auto& isolatedHw) {
            return ((isolatedHw.second->getEntityPath() == entityPath) &&
                    (isolatedHw.second->getEntryRecId() == recordId));
        });

    if (isolatedHwIt == _isolatedHardwares.end())
    {
        // D-Bus entry does not exist
        return std::make_pair(false, std::string());
    }

    // Add association for isolated hardware inventory path
    // Note: Association forward and reverse type are defined as per
    // hardware isolation design document (aka guard) and hardware isolation
    // entry dbus interface document for hardware and error object path
    type::AsscDefFwdType isolateHwFwdType("isolated_hw");
    type::AsscDefRevType isolatedHwRevType("isolated_hw_entry");
    type::AssociationDef associationDeftoHw;
    associationDeftoHw.push_back(std::make_tuple(
        isolateHwFwdType, isolatedHwRevType, isolatedHwDbusObjPath));

    // Add errog log as Association if given
    if (!bmcErrorLog.empty())
    {
        type::AsscDefFwdType bmcErrorLogFwdType("isolated_hw_errorlog");
        type::AsscDefRevType bmcErrorLogRevType("isolated_hw_entry");
        associationDeftoHw.push_back(std::make_tuple(
            bmcErrorLogFwdType, bmcErrorLogRevType, bmcErrorLog));
    }

    // Existing record might be overridden in the libguard during
    // creation if that's meets certain override conditions
    bool updated{false};
    if (isolatedHwIt->second->severity() != severity)
    {
        isolatedHwIt->second->severity(severity);
        updated = true;
    }

    if (isolatedHwIt->second->associations() != associationDeftoHw)
    {
        isolatedHwIt->second->associations(associationDeftoHw);
        updated = true;
    }

    if (updated)
    {
        // Existing entry might be overwritten if that's meets certain
        // overwritten conditions so update creation time.
        std::time_t timeStamp = std::time(nullptr);
        isolatedHwIt->second->elapsed(timeStamp);
    }

    auto entryObjPath = fs::path(HW_ISOLATION_ENTRY_OBJPATH) /
                        std::to_string(isolatedHwIt->first);

    isolatedHwIt->second->serialize();
    return std::make_pair(true, entryObjPath.string());
}

void Manager::isHwIsolationAllowed(const entry::EntrySeverity& severity)
{
    // Make sure the hardware isolation setting is enabled or not
    if (!utils::isHwIosolationSettingEnabled(_bus))
    {
        log<level::INFO>(
            fmt::format("Hardware isolation is not allowed "
                        "since the HardwareIsolation setting is disabled")
                .c_str());
        throw type::CommonError::Unavailable();
    }

    if (severity == entry::EntrySeverity::Manual)
    {
        using Chassis = sdbusplus::xyz::openbmc_project::State::server::Chassis;

        auto systemPowerState = utils::getDBusPropertyVal<std::string>(
            _bus, "/xyz/openbmc_project/state/chassis0",
            "xyz.openbmc_project.State.Chassis", "CurrentPowerState");

        if (Chassis::convertPowerStateFromString(systemPowerState) !=
            Chassis::PowerState::Off)
        {
            log<level::ERR>(fmt::format("Manual hardware isolation is allowed "
                                        "only when chassis powerstate is off")
                                .c_str());
            throw type::CommonError::NotAllowed();
        }
    }
}

sdbusplus::message::object_path Manager::create(
    sdbusplus::message::object_path isolateHardware,
    sdbusplus::xyz::openbmc_project::HardwareIsolation::server::Entry::Type
        severity)
{
    isHwIsolationAllowed(severity);

    auto devTreePhysicalPath = _isolatableHWs.getPhysicalPath(isolateHardware);
    if (!devTreePhysicalPath.has_value())
    {
        log<level::ERR>(fmt::format("Invalid argument [IsolateHardware: {}]",
                                    isolateHardware.str)
                            .c_str());
        throw type::CommonError::InvalidArgument();
    }

    auto guardType = entry::utils::getGuardType(severity);
    if (!guardType.has_value())
    {
        log<level::ERR>(
            fmt::format("Invalid argument [Severity: {}]",
                        entry::EntryInterface::convertTypeToString(severity))
                .c_str());
        throw type::CommonError::InvalidArgument();
    }

    auto guardRecord = openpower_guard::create(
        openpower_guard::EntityPath(devTreePhysicalPath->data(),
                                    devTreePhysicalPath->size()),
        0, *guardType);

    if (auto ret = updateEntry(guardRecord->recordId, severity,
                               isolateHardware.str, "", guardRecord->targetId);
        ret.first == true)
    {
        return ret.second;
    }
    else
    {
        auto entryPath =
            createEntry(guardRecord->recordId, false, severity,
                        isolateHardware.str, "", true, guardRecord->targetId);

        if (!entryPath.has_value())
        {
            throw type::CommonError::InternalFailure();
        }
        return *entryPath;
    }
}

sdbusplus::message::object_path Manager::createWithErrorLog(
    sdbusplus::message::object_path isolateHardware,
    sdbusplus::xyz::openbmc_project::HardwareIsolation::server::Entry::Type
        severity,
    sdbusplus::message::object_path bmcErrorLog)
{
    isHwIsolationAllowed(severity);

    auto devTreePhysicalPath = _isolatableHWs.getPhysicalPath(isolateHardware);
    if (!devTreePhysicalPath.has_value())
    {
        log<level::ERR>(fmt::format("Invalid argument [IsolateHardware: {}]",
                                    isolateHardware.str)
                            .c_str());
        throw type::CommonError::InvalidArgument();
    }

    auto eId = getEID(bmcErrorLog);
    if (!eId.has_value())
    {
        log<level::ERR>(
            fmt::format("Invalid argument [BmcErrorLog: {}]", bmcErrorLog.str)
                .c_str());
        throw type::CommonError::InvalidArgument();
    }

    auto guardType = entry::utils::getGuardType(severity);
    if (!guardType.has_value())
    {
        log<level::ERR>(
            fmt::format("Invalid argument [Severity: {}]",
                        entry::EntryInterface::convertTypeToString(severity))
                .c_str());
        throw type::CommonError::InvalidArgument();
    }

    auto guardRecord = openpower_guard::create(
        openpower_guard::EntityPath(devTreePhysicalPath->data(),
                                    devTreePhysicalPath->size()),
        *eId, *guardType);

    if (auto ret =
            updateEntry(guardRecord->recordId, severity, isolateHardware.str,
                        bmcErrorLog.str, guardRecord->targetId);
        ret.first == true)
    {
        return ret.second;
    }
    else
    {
        auto entryPath = createEntry(guardRecord->recordId, false, severity,
                                     isolateHardware.str, bmcErrorLog.str, true,
                                     guardRecord->targetId);

        if (!entryPath.has_value())
        {
            throw type::CommonError::InternalFailure();
        }
        return *entryPath;
    }
}

void Manager::eraseEntry(const entry::EntryRecordId entryRecordId)
{
    if (_isolatedHardwares.contains(entryRecordId))
    {
        updateEcoCoresList(
            false, devtree::convertEntityPathIntoRawData(
                       _isolatedHardwares.at(entryRecordId)->getEntityPath()));
    }
    _isolatedHardwares.erase(entryRecordId);
}

void Manager::resolveAllEntries(bool clearRecord)
{
    auto entryIt = _isolatedHardwares.begin();
    while (entryIt != _isolatedHardwares.end())
    {
        auto entryRecordId = entryIt->first;
        auto& entry = entryIt->second;
        std::advance(entryIt, 1);

        // Continue other entries to delete if failed to delete one entry
        try
        {
            entry->resolveEntry(clearRecord);
        }
        catch (std::exception& e)
        {
            log<level::ERR>(fmt::format("Exception [{}] to delete entry [{}]",
                                        e.what(), entryRecordId)
                                .c_str());
        }
    }
}

void Manager::deleteAll()
{
    // throws exception if not allowed
    hw_isolation::utils::isHwDeisolationAllowed(_bus);

    resolveAllEntries();
}

bool Manager::isValidRecord(const entry::EntryRecordId recordId)
{
    if (recordId != 0xFFFFFFFF)
    {
        return true;
    }

    return false;
}

void Manager::createEntryForRecord(const openpower_guard::GuardRecord& record,
                                   const bool isRestorePath)
{
    auto entityPathRawData =
        devtree::convertEntityPathIntoRawData(record.targetId);
    std::stringstream ss;
    std::for_each(entityPathRawData.begin(), entityPathRawData.end(),
                  [&ss](const auto& ele) {
                      ss << std::setw(2) << std::setfill('0') << std::hex
                         << (int)ele << " ";
                  });

    try
    {
        entry::EntryResolved resolved = false;
        if (record.recordId == 0xFFFFFFFF)
        {
            resolved = true;
        }

        bool ecoCore{
            (_persistedEcoCores.contains(entityPathRawData) && isRestorePath)};

        auto isolatedHwInventoryPath =
            _isolatableHWs.getInventoryPath(entityPathRawData, ecoCore);

        if (!isolatedHwInventoryPath.has_value())
        {
            log<level::ERR>(
                fmt::format(
                    "Skipping to restore a given isolated "
                    "hardware [{}] : Due to failure to get inventory path",
                    ss.str())
                    .c_str());
            return;
        }
        updateEcoCoresList(ecoCore, entityPathRawData);

        auto bmcErrorLogPath = utils::getBMCLogPath(_bus, record.elogId);
        std::string strBmcErrorLogPath{};
        if (!bmcErrorLogPath.has_value())
        {
            if (!isRestorePath)
            {
                log<level::ERR>(
                    fmt::format("Skipping to restore a given isolated "
                                "hardware [{}] : Due to failure to get BMC "
                                "error log path "
                                "by isolated hardware EID (aka PEL ID) [{:#X}]",
                                ss.str(), record.elogId)
                        .c_str());
                return;
            }
        }
        else
        {
            strBmcErrorLogPath = bmcErrorLogPath->str;
        }

        auto entrySeverity = entry::utils::getEntrySeverityType(
            static_cast<openpower_guard::GardType>(record.errType));
        if (!entrySeverity.has_value())
        {
            log<level::ERR>(
                fmt::format("Skipping to restore a given isolated "
                            "hardware [{}] : Due to failure to to get BMC "
                            "EntrySeverity by isolated hardware GardType [{}]",
                            ss.str(), record.errType)
                    .c_str());
            return;
        }

        auto entryPath =
            createEntry(record.recordId, resolved, *entrySeverity,
                        isolatedHwInventoryPath->str, strBmcErrorLogPath, false,
                        record.targetId);

        if (!entryPath.has_value())
        {
            log<level::ERR>(
                fmt::format(
                    "Skipping to restore a given isolated "
                    "hardware [{}] : Due to failure to create dbus entry",
                    ss.str())
                    .c_str());
            return;
        }
    }
    catch (const std::exception& e)
    {
        log<level::ERR>(
            fmt::format("Exception [{}] : Skipping to restore a given isolated "
                        "hardware [{}]",
                        e.what(), ss.str())
                .c_str());
    }
}

void Manager::updateEntryForRecord(const openpower_guard::GuardRecord& record,
                                   IsolatedHardwares::iterator& entryIt)
{
    auto entityPathRawData =
        devtree::convertEntityPathIntoRawData(record.targetId);
    std::stringstream ss;
    std::for_each(entityPathRawData.begin(), entityPathRawData.end(),
                  [&ss](const auto& ele) {
                      ss << std::setw(2) << std::setfill('0') << std::hex
                         << (int)ele << " ";
                  });

    bool ecoCore{false};

    auto isolatedHwInventoryPath =
        _isolatableHWs.getInventoryPath(entityPathRawData, ecoCore);

    if (!isolatedHwInventoryPath.has_value())
    {
        log<level::ERR>(
            fmt::format("Skipping to restore a given isolated "
                        "hardware [{}] : Due to failure to get inventory path",
                        ss.str())
                .c_str());
        return;
    }
    updateEcoCoresList(ecoCore, entityPathRawData);

    auto bmcErrorLogPath = utils::getBMCLogPath(_bus, record.elogId);

    if (!bmcErrorLogPath.has_value())
    {
        log<level::ERR>(
            fmt::format(
                "Skipping to restore a given isolated "
                "hardware [{}] : Due to failure to get BMC error log path "
                "by isolated hardware EID (aka PEL ID) [{}]",
                ss.str(), record.elogId)
                .c_str());
        return;
    }

    auto entrySeverity = entry::utils::getEntrySeverityType(
        static_cast<openpower_guard::GardType>(record.errType));
    if (!entrySeverity.has_value())
    {
        log<level::ERR>(
            fmt::format("Skipping to restore a given isolated "
                        "hardware [{}] : Due to failure to to get BMC "
                        "EntrySeverity by isolated hardware GardType [{}]",
                        ss.str(), record.errType)
                .c_str());
        return;
    }

    // Add association for isolated hardware inventory path
    // Note: Association forward and reverse type are defined as per
    // hardware isolation design document (aka guard) and hardware isolation
    // entry dbus interface document for hardware and error object path
    type::AsscDefFwdType isolateHwFwdType("isolated_hw");
    type::AsscDefRevType isolatedHwRevType("isolated_hw_entry");
    type::AssociationDef associationDeftoHw;
    associationDeftoHw.push_back(std::make_tuple(
        isolateHwFwdType, isolatedHwRevType, *isolatedHwInventoryPath));

    // Add errog log as Association if given
    if (!bmcErrorLogPath->str.empty())
    {
        type::AsscDefFwdType bmcErrorLogFwdType("isolated_hw_errorlog");
        type::AsscDefRevType bmcErrorLogRevType("isolated_hw_entry");
        associationDeftoHw.push_back(std::make_tuple(
            bmcErrorLogFwdType, bmcErrorLogRevType, *bmcErrorLogPath));
    }

    bool updated{false};
    if (entryIt->second->severity() != entrySeverity)
    {
        entryIt->second->severity(*entrySeverity);
        updated = true;
    }

    if (entryIt->second->associations() != associationDeftoHw)
    {
        entryIt->second->associations(associationDeftoHw);
        updated = true;
    }

    utils::setEnabledProperty(_bus, *isolatedHwInventoryPath, false);

    if (updated)
    {
        // Existing entry might be overwritten if that's meets certain
        // overwritten conditions so update creation time.
        std::time_t timeStamp = std::time(nullptr);
        entryIt->second->elapsed(timeStamp);
    }

    entryIt->second->serialize();
}

void Manager::cleanupPersistedEcoCores()
{
    bool updated{false};
    if (_isolatedHardwares.empty())
    {
        _persistedEcoCores.clear();
        updated = true;
    }
    else
    {
        for (auto ecoCore = _persistedEcoCores.begin();
             ecoCore != _persistedEcoCores.end();)
        {
            auto nextEcoCore = std::next(ecoCore, 1);

            auto isNotIsolated = std::ranges::none_of(
                _isolatedHardwares, [ecoCore](const auto& entry) {
                    return (entry.second->getEntityPath() ==
                            openpower_guard::EntityPath(ecoCore->data(),
                                                        ecoCore->size()));
                });

            if (isNotIsolated)
            {
                updateEcoCoresList(false, *ecoCore);
                updated = true;
            }

            ecoCore = nextEcoCore;
        }
    }

    if (updated)
    {
        serialize();
    }
}

void Manager::cleanupPersistedFiles()
{
    auto deletePersistedEntryFileIfNotExist = [this](const auto& file) {
        auto fileEntryId = std::stoul(file.path().filename());

        if (!(this->_isolatedHardwares.contains(fileEntryId)))
        {
            fs::remove(file.path());
        }
    };

    std::ranges::for_each(
        fs::directory_iterator(
            fs::path(HW_ISOLATION_ENTRY_PERSIST_PATH).parent_path()),
        deletePersistedEntryFileIfNotExist);

    cleanupPersistedEcoCores();
}

void Manager::restore()
{
    // Don't get ephemeral records (GARD_Reconfig and GARD_Sticky_deconfig
    // because those type records are created for internal purpose to use
    // by BMC and Hostboot
    openpower_guard::GuardRecords records = openpower_guard::getAll(true);

    auto validRecord = [this](const auto& record) {
        return this->isValidRecord(record.recordId);
    };

    auto validRecords = records | std::views::filter(validRecord);

    auto createEntry = [this](const auto& record) {
        this->createEntryForRecord(record, true);
    };

    std::ranges::for_each(validRecords, createEntry);

    cleanupPersistedFiles();
}

void Manager::processHardwareIsolationRecordFile()
{
    /**
     * Start timer in the event loop to get the final isolated hardware
     * record list which are updated by the host because of the atomicity
     * on the partition file (which is used to store isolated hardware details)
     * between BMC and Host.
     */
    try
    {
        _timerObjs.emplace(
            std::make_unique<
                sdeventplus::utility::Timer<sdeventplus::ClockId::Monotonic>>(
                _eventLoop,
                std::bind(std::mem_fn(&hw_isolation::record::Manager::
                                          handleHostIsolatedHardwares),
                          this),
                std::chrono::seconds(5)));
    }
    catch (const std::exception& e)
    {
        log<level::ERR>(
            fmt::format("Exception [{}], Failed to process "
                        "hardware isolation record file that's updated",
                        e.what())
                .c_str());
    }
}

void Manager::handleHostIsolatedHardwares()
{
    auto timerObj = std::move(_timerObjs.front());
    _timerObjs.pop();
    if (timerObj->isEnabled())
    {
        timerObj->setEnabled(false);
    }

    // Don't get ephemeral records (GARD_Reconfig and GARD_Sticky_deconfig
    // because those type records are created for internal purpose to use
    // by BMC and Hostboot
    openpower_guard::GuardRecords records = openpower_guard::getAll(true);

    // Delete all the D-Bus entries if no record in their persisted location
    if ((records.size() == 0) && _isolatedHardwares.size() > 0)
    {
        // Clean up all entries association before delete.
        resolveAllEntries(false);
        _isolatedHardwares.clear();
        return;
    }

    auto validRecord = [this](const auto& record) {
        return this->isValidRecord(record.recordId);
    };

    for (auto entryIt = _isolatedHardwares.begin();
         entryIt != _isolatedHardwares.end();)
    {
        auto nextEntryIt = std::next(entryIt, 1);

        auto entryRecord = [entryIt](const auto& record) {
            return entryIt->second->getEntityPath() == record.targetId;
        };
        auto entryRecords = records | std::views::filter(entryRecord);

        if (entryRecords.empty())
        {
            entryIt->second->resolveEntry(false);
        }
        else
        {
            auto validEntryRecords =
                entryRecords | std::views::filter(validRecord);

            if (validEntryRecords.empty())
            {
                entryIt->second->resolveEntry(false);
            }
            else if (std::distance(validEntryRecords.begin(),
                                   validEntryRecords.end()) == 1)
            {
                this->updateEntryForRecord(validEntryRecords.front(), entryIt);
            }
            else
            {
                // Should not happen since, more than one valid records
                // for the same hardware is not allowed
                auto entityPathRawData = devtree::convertEntityPathIntoRawData(
                    entryIt->second->getEntityPath());
                std::stringstream ss;
                std::for_each(entityPathRawData.begin(),
                              entityPathRawData.end(), [&ss](const auto& ele) {
                                  ss << std::setw(2) << std::setfill('0')
                                     << std::hex << (int)ele << " ";
                              });
                log<level::ERR>(fmt::format("More than one valid records exist "
                                            "for the same hardware [{}]",
                                            ss.str())
                                    .c_str());
            }
        }
        entryIt = nextEntryIt;
    }

    auto validRecords = records | std::views::filter(validRecord);

    auto createEntryIfNotExists = [this](const auto& validRecord) {
        auto recordExist = [validRecord](const auto& entry) {
            return validRecord.targetId == entry.second->getEntityPath();
        };

        if (std::ranges::none_of(this->_isolatedHardwares, recordExist))
        {
            this->createEntryForRecord(validRecord);
        }
    };

    std::ranges::for_each(validRecords, createEntryIfNotExists);

    cleanupPersistedEcoCores();
}

sdbusplus::message::object_path Manager::createWithEntityPath(
    std::vector<uint8_t> entityPath,
    sdbusplus::xyz::openbmc_project::HardwareIsolation::server::Entry::Type
        severity,
    sdbusplus::message::object_path bmcErrorLog)
{
    isHwIsolationAllowed(severity);

    bool ecoCore{false};

    auto isolateHwInventoryPath =
        _isolatableHWs.getInventoryPath(entityPath, ecoCore);

    std::stringstream ss;
    std::for_each(entityPath.begin(), entityPath.end(), [&ss](const auto& ele) {
        ss << std::setw(2) << std::setfill('0') << std::hex << (int)ele << " ";
    });
    if (!isolateHwInventoryPath.has_value())
    {
        log<level::ERR>(
            fmt::format("Invalid argument [IsolateHardware: {}]", ss.str())
                .c_str());
        throw type::CommonError::InvalidArgument();
    }
    updateEcoCoresList(ecoCore, entityPath);

    auto eId = getEID(bmcErrorLog);
    if (!eId.has_value())
    {
        log<level::ERR>(
            fmt::format("Invalid argument [BmcErrorLog: {}]", bmcErrorLog.str)
                .c_str());
        throw type::CommonError::InvalidArgument();
    }

    auto guardType = entry::utils::getGuardType(severity);
    if (!guardType.has_value())
    {
        log<level::ERR>(
            fmt::format("Invalid argument [Severity: {}]",
                        entry::EntryInterface::convertTypeToString(severity))
                .c_str());
        throw type::CommonError::InvalidArgument();
    }

    auto guardRecord = openpower_guard::create(
        openpower_guard::EntityPath(entityPath.data(), entityPath.size()), *eId,
        *guardType);

    if (auto ret = updateEntry(guardRecord->recordId, severity,
                               isolateHwInventoryPath->str, bmcErrorLog.str,
                               guardRecord->targetId);
        ret.first == true)
    {
        return ret.second;
    }
    else
    {
        auto entryPath = createEntry(
            guardRecord->recordId, false, severity, isolateHwInventoryPath->str,
            bmcErrorLog.str, true, guardRecord->targetId);

        if (!entryPath.has_value())
        {
            throw type::CommonError::InternalFailure();
        }
        return *entryPath;
    }
}

std::optional<std::tuple<entry::EntrySeverity, entry::EntryErrLogPath>>
    Manager::getIsolatedHwRecordInfo(
        const sdbusplus::message::object_path& hwInventoryPath)
{
    // Make sure whether the given hardware inventory is exists
    // in the record list.
    auto entryIt = std::find_if(
        _isolatedHardwares.begin(), _isolatedHardwares.end(),
        [hwInventoryPath](const auto& ele) {
            for (const auto& assocEle : ele.second->associations())
            {
                if (std::get<0>(assocEle) == "isolated_hw")
                {
                    return std::get<2>(assocEle) == hwInventoryPath.str;
                }
            }

            return false;
        });

    if (entryIt == _isolatedHardwares.end())
    {
        return std::nullopt;
    }

    entry::EntryErrLogPath errLogPath;
    for (const auto& assocEle : entryIt->second->associations())
    {
        if (std::get<0>(assocEle) == "isolated_hw_errorlog")
        {
            errLogPath = std::get<2>(assocEle);
            break;
        }
    }

    return std::make_tuple(entryIt->second->severity(), errLogPath);
}

} // namespace record
} // namespace hw_isolation
