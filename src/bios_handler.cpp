#include "bios_handler.hpp"

#include "constants.hpp"
#include "logger.hpp"

#include <sdbusplus/bus/match.hpp>
#include <utility/dbus_utility.hpp>

namespace vpd
{
// Template declaration to define APIs.
template class BiosHandler<IbmBiosHandler>;

template <typename T>
void BiosHandler<T>::checkAndListenPldmService()
{
    // Setup a call back match on NameOwnerChanged to determine when PLDM is up.
    static std::shared_ptr<sdbusplus::bus::match_t> l_nameOwnerMatch =
        std::make_shared<sdbusplus::bus::match_t>(
            *m_asioConn,
            sdbusplus::bus::match::rules::nameOwnerChanged(
                constants::pldmServiceName),
            [this](sdbusplus::message_t& l_msg) {
        if (l_msg.is_method_error())
        {
            logging::logMessage(
                "Error in reading PLDM name owner changed signal.");
            return;
        }

        std::string l_name;
        std::string l_newOwner;
        std::string l_oldOwner;

        l_msg.read(l_name, l_oldOwner, l_newOwner);

        if (!l_newOwner.empty() &&
            (l_name.compare(constants::pldmServiceName) ==
             constants::STR_CMP_SUCCESS))
        {
            // TODO: Restore BIOS attribute from here.
            //  We don't need the match anymore
            l_nameOwnerMatch.reset();
            m_specificBiosHandler->backUpOrRestoreBiosAttributes();

            // Start listener now that we have done the restore.
            listenBiosAttributes();
        }
    });

    // Based on PLDM service status reset owner match registered above and
    // trigger BIOS attribute sync.
    if (dbusUtility::isServiceRunning(constants::pldmServiceName))
    {
        l_nameOwnerMatch.reset();
        m_specificBiosHandler->backUpOrRestoreBiosAttributes();

        // Start listener now that we have done the restore.
        listenBiosAttributes();
    }
}

template <typename T>
void BiosHandler<T>::listenBiosAttributes()
{
    static std::shared_ptr<sdbusplus::bus::match_t> l_biosMatch =
        std::make_shared<sdbusplus::bus::match_t>(
            *m_asioConn,
            sdbusplus::bus::match::rules::propertiesChanged(
                constants::biosConfigMgrObjPath,
                constants::biosConfigMgrInterface),
            [this](sdbusplus::message_t& l_msg) {
        m_specificBiosHandler->biosAttributesCallback(l_msg);
    });
}

void IbmBiosHandler::biosAttributesCallback(sdbusplus::message_t& i_msg)
{
    if (i_msg.is_method_error())
    {
        logging::logMessage("Error in reading BIOS attribute signal. ");
        return;
    }

    std::string l_objPath;
    types::BiosBaseTableType l_propMap;
    i_msg.read(l_objPath, l_propMap);

    for (auto l_property : l_propMap)
    {
        if (l_property.first != "BaseBIOSTable")
        {
            // Looking for change in Base BIOS table only.
            continue;
        }

        auto l_attributeList = std::get<0>(l_property.second);

        for (const auto& l_attribute : l_attributeList)
        {
            if (auto l_val = std::get_if<std::string>(
                    &(std::get<5>(std::get<1>(l_attribute)))))
            {
                std::string l_attributeName = std::get<0>(l_attribute);
                if (l_attributeName == "hb_memory_mirror_mode")
                {
                    saveAmmToVpd(*l_val);
                }

                if (l_attributeName == "pvm_keep_and_clear")
                {
                    // TODO: Save keep and clear to VPD.
                }

                if (l_attributeName == "pvm_create_default_lpar")
                {
                    // TODO: Save lpar to VPD.
                }

                if (l_attributeName == "pvm_clear_nvram")
                {
                    // TODO: Save clear nvram to VPD.
                }

                continue;
            }

            if (auto l_val = std::get_if<int64_t>(
                    &(std::get<5>(std::get<1>(l_attribute)))))
            {
                std::string l_attributeName = std::get<0>(l_attribute);
                if (l_attributeName == "hb_field_core_override")
                {
                    saveFcoToVpd(*l_val);
                }
            }
        }
    }
}

void IbmBiosHandler::backUpOrRestoreBiosAttributes()
{
    // process FCO
    processFieldCoreOverride();
}

types::BiosAttributeCurrentValue
    IbmBiosHandler::readBiosAttribute(const std::string& i_attributeName)
{
    types::BiosAttributeCurrentValue l_attrValueVariant =
        dbusUtility::biosGetAttributeMethodCall(i_attributeName);

    return l_attrValueVariant;
}

void IbmBiosHandler::processFieldCoreOverride()
{
    // TODO: Should we avoid doing this at runtime?

    // Read required keyword from Dbus.
    auto l_kwdValueVariant = dbusUtility::readDbusProperty(
        constants::pimServiceName, constants::systemVpdInvPath,
        constants::vsysInf, constants::kwdRG);

    if (auto l_fcoInVpd = std::get_if<types::BinaryVector>(&l_kwdValueVariant))
    {
        // default length of the keyword is 4 bytes.
        if (l_fcoInVpd->size() != constants::VALUE_4)
        {
            logging::logMessage(
                "Invalid value read for FCO from D-Bus. Skipping.");
        }

        //  If FCO in VPD contains anything other that ASCII Space, restore to
        //  BIOS
        if (std::any_of(l_fcoInVpd->cbegin(), l_fcoInVpd->cend(),
                        [](uint8_t l_val) {
            return l_val != constants::ASCII_OF_SPACE;
        }))
        {
            // Restore the data to BIOS.
            // TODO: saveFcoToBios(*l_fcoInVpd);
        }
        else
        {
            types::BiosAttributeCurrentValue l_attrValueVariant =
                readBiosAttribute("hb_field_core_override");

            if (auto l_fcoInBios = std::get_if<int64_t>(&l_attrValueVariant))
            {
                // save the BIOS data to VPD
                saveFcoToVpd(*l_fcoInBios);

                return;
            }
            logging::logMessage("Invalid type recieved for FCO from BIOS.");
        }
        return;
    }
    logging::logMessage("Invalid type recieved for FCO from VPD.");
}

void IbmBiosHandler::saveFcoToVpd(int64_t i_fcoInBios)
{
    if (i_fcoInBios < 0)
    {
        logging::logMessage("Invalid FCO value in BIOS. Skip updating to VPD");
        return;
    }

    // convert to VPD value type
    types::BinaryVector l_biosValInVpdFormat = {
        0, 0, 0, static_cast<uint8_t>(i_fcoInBios)};

    // TODO:  Call Manager API to write keyword data using inventory path.
}

void IbmBiosHandler::saveAmmToVpd(const std::string& i_memoryMirrorMode)
{
    if (i_memoryMirrorMode.empty())
    {
        logging::logMessage(
            "Empty memory mirror mode value from BIOS. Skip writing to VPD");
        return;
    }

    types::BinaryVector l_valToUpdateInVpd{
        (i_memoryMirrorMode == "Enabled" ? constants::AMM_ENABLED_IN_VPD
                                         : constants::AMM_DISABLED_IN_VPD)};

    // TODO: Call write keyword API to update the value in VPD.
}
} // namespace vpd
