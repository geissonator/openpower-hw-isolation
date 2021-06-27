// SPDX-License-Identifier: Apache-2.0

#include "utils.hpp"

namespace hw_isolation
{
namespace utils
{

std::string getDBusServiceName(sdbusplus::bus::bus& bus,
                               const std::string& path,
                               const std::string& interface)
{
    constexpr auto ObjectMapperName = "xyz.openbmc_project.ObjectMapper";
    constexpr auto ObjectMapperPath = "/xyz/openbmc_project/object_mapper";

    std::vector<std::pair<std::string, std::vector<std::string>>> servicesName;

    try
    {
        auto method = bus.new_method_call(ObjectMapperName, ObjectMapperPath,
                                          ObjectMapperName, "GetObject");

        method.append(path);
        method.append(std::vector<std::string>({interface}));

        auto reply = bus.call(method);
        reply.read(servicesName);
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        log<level::ERR>(fmt::format("Exception [{}] to get dbus service name "
                                    "for object [{}] and interface [{}]",
                                    e.what(), path, interface)
                            .c_str());
        throw sdbusplus::exception::SdBusError(
            const_cast<sd_bus_error*>(e.get_error()), "HW-Isolation");
    }

    // In OpenBMC, the object path will be hosted by a single service
    // i.e more than one service cannot host the same object path.
    if (servicesName.size() > 1)
    {
        std::string serviceNameList{""};

        std::for_each(servicesName.begin(), servicesName.end(),
                      [&serviceNameList](const auto& serviceName) {
                          serviceNameList.append(serviceName.first + ",");
                      });

        log<level::ERR>(fmt::format("The given object path hosted by "
                                    "more than one services [{}]",
                                    serviceNameList)
                            .c_str());
        throw std::runtime_error(
            "Given object path hosted by more than one service");
    }

    return servicesName[0].first;
}

} // namespace utils
} // namespace hw_isolation
