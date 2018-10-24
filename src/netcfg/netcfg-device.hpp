//  OpenVPN 3 Linux client -- Next generation OpenVPN client
//
//  Copyright (C) 2018         OpenVPN, Inc. <sales@openvpn.net>
//  Copyright (C) 2018         David Sommerseth <davids@openvpn.net>
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU Affero General Public License as
//  published by the Free Software Foundation, version 3 of the
//  License.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU Affero General Public License for more details.
//
//  You should have received a copy of the GNU Affero General Public License
//  along with this program.  If not, see <https://www.gnu.org/licenses/>.
//

/**
 * @file   netcfg-device.hpp
 *
 * @brief  D-Bus object representing a single virtual network device
 *         the net.openvpn.v3.netcfg service manages
 */

#pragma once

#include <functional>
#include <gio-unix-2.0/gio/gunixfdlist.h>

#include <openvpn/common/rc.hpp>

#include "dbus/core.hpp"
#include "dbus/connection-creds.hpp"
#include "dbus/glibutils.hpp"
#include "dbus/object-property.hpp"
#include "ovpn3cli/lookup.hpp"
#include "./dns-direct-file.hpp"
#include "netcfg-stateevent.hpp"
#include "netcfg-signals.hpp"

using namespace openvpn;
using namespace NetCfg;

enum class NetCfgDeviceType
{
    UNSET,   // Primarily to avoid 0 but still have 0 defined
    TUN,
    TAP
};

class NetCfgDevice : public DBusObject,
                     public DBusCredentials
{
public:
    NetCfgDevice(GDBusConnection *dbuscon,
                 std::function<void()> remove_callback,
                 const uid_t creator, const std::string& objpath,
                 const NetCfgDeviceType& devtype, const std::string devname,
                 DNS::ResolverSettings *resolver,
                 const unsigned int log_level, LogWriter *logwr)
        : DBusObject(objpath),
          DBusCredentials(dbuscon, creator),
          remove_callback(std::move(remove_callback)),
          properties(this),
          device_type(devtype),
          device_name(std::move(devname)),
          mtu(1500),
          signal(dbuscon, LogGroup::NETCFG, objpath, logwr),
          resolver(resolver)
    {
        signal.SetLogLevel(log_level);

        properties.AddBinding(new PropertyType<std::string>(this, "device_name", "read", false, device_name));
        properties.AddBinding(new PropertyType<decltype(dns_servers)>(this, "dns_servers", "read", false, dns_servers));
        properties.AddBinding(new PropertyType<decltype(dns_search)>(this, "dns_search", "read", false, dns_search));
        properties.AddBinding(new PropertyType<unsigned int>(this, "mtu", "readwrite", false, mtu));
        //properties.AddBinding(new PropertyType<NetCfgDeviceType>(this, "layer", "read", false, device_type));

        std::stringstream introspect;
        introspect << "<node name='" << objpath << "'>"
                   << "    <interface name='" << OpenVPN3DBus_interf_netcfg << "'>"
                   << "        <method name='AddIPv4Address'>"
                   << "            <arg direction='in' type='s' name='ip_address'/>"
                   << "            <arg direction='in' type='u' name='prefix'/>"
                   << "        </method>"
                   << "        <method name='RemoveIPv4Address'>"
                   << "            <arg direction='in' type='s' name='ip_address'/>"
                   << "            <arg direction='in' type='u' name='prefix'/>"
                   << "        </method>"
                   << "        <method name='AddIPv6Address'>"
                   << "            <arg direction='in' type='s' name='ip_address'/>"
                   << "            <arg direction='in' type='u' name='prefix'/>"
                   << "        </method>"
                   << "        <method name='RemoveIPv6Address'>"
                   << "            <arg direction='in' type='s' name='ip_address'/>"
                   << "            <arg direction='in' type='u' name='prefix'/>"
                   << "        </method>"
                   << "        <method name='AddRoutes'>"
                   << "            <arg direction='in' type='as' name='route_target'/>"
                   << "            <arg direction='in' type='s' name='gateway'/>"
                   << "        </method>"
                   << "        <method name='RemoveRoutes'>"
                   << "            <arg direction='in' type='as' name='route_target'/>"
                   << "            <arg direction='in' type='s' name='gateway'/>"
                   << "        </method>"
                   << "        <method name='AddDNS'>"
                   << "            <arg direction='in' type='as' name='server_list'/>"
                   << "        </method>"
                   << "        <method name='RemoveDNS'>"
                   << "            <arg direction='in' type='as' name='server_list'/>"
                   << "        </method>"
                   << "        <method name='AddDNSSearch'>"
                   << "            <arg direction='in' type='as' name='domains'/>"
                   << "        </method>"
                   << "        <method name='RemoveDNSSearch'>"
                   << "            <arg direction='in' type='as' name='domains'/>"
                   << "        </method>"
                   << "        <method name='Establish'/>"
                                /* Note: Although Establish returns a unix_fd, it does not belong in
                                 * the function signature, since glib/dbus abstraction is paper thin
                                 * and it is handled almost like in recv/sendmsg as auxiliary data
                                 */
                   << "        <method name='Disable'/>"
                   << "        <method name='Destroy'/>"
                   << "        <property type='u'  name='log_level' access='readwrite'/>"
                   << "        <property type='u'  name='owner' access='read'/>"
                   << "        <property type='au' name='acl' access='read'/>"
                   << "        <property type='b'  name='active' access='read'/>"
                   << "        <property type='b'  name='modified' access='read'/>"
                   << "        <property type='as' name='ipv4_addresses' access='read'/>"
                   << "        <property type='as' name='ipv4_routes' access='read'/>"
                   << "        <property type='as' name='ipv6_addresses' access='read'/>"
                   << "        <property type='as' name='ipv6_routes' access='read'/>"
                   << properties.GetIntrospectionXML()
                   << signal.GetLogIntrospection()
                   << NetCfgStateEvent::IntrospectionXML()
                   << "    </interface>"
                   << "</node>";
        ParseIntrospectionXML(introspect);
        signal.LogVerb2("Network device '" + devname + "' prepared");

        // Increment the device reference counter in the resolver
        if (resolver)
        {
            resolver->IncDeviceCount();
        }
    }

    ~NetCfgDevice()
    {
        remove_callback();
        IdleCheck_RefDec();
    }


    /**
     *  Callback method which is called each time a D-Bus method call occurs
     *  on this BackendClientObject.
     *
     * @param conn        D-Bus connection where the method call occurred
     * @param sender      D-Bus bus name of the sender of the method call
     * @param obj_path    D-Bus object path of the target object.
     * @param intf_name   D-Bus interface of the method call
     * @param method_name D-Bus method name to be executed
     * @param params      GVariant Glib2 object containing the arguments for
     *                    the method call
     * @param invoc       GDBusMethodInvocation where the response/result of
     *                    the method call will be returned.
     */
    void callback_method_call(GDBusConnection *conn,
                              const std::string sender,
                              const std::string obj_path,
                              const std::string intf_name,
                              const std::string method_name,
                              GVariant *params,
                              GDBusMethodInvocation *invoc)
    {
        try
        {
            IdleCheck_UpdateTimestamp();

            // Only the VPN backend clients are granted access
            validate_sender(sender);

            GVariant *retval = nullptr;
            if ("AddIPv4Address" == method_name)
            {
                // Adds a single IPv4 address to the virtual device.  If
                // broadcast has not been provided, calculate it if needed.
            }
            else if ("RemoveIPv4Address" == method_name)
            {
                // Removes a single IPv4 address from the virtual device
            }
            else if ("AddIPv6Address" == method_name)
            {
                // Adds a single IPv6 address to the virtual device
            }
            if ("RemoveIPv6Address" == method_name)
            {
                // Removes a single IPv6 address from the virtual device
            }
            else if ("AddRoutes" == method_name)
            {
                // The caller sends an array of routes to apply
                // It is an array, as this makes everything happen in a
                // single D-Bus method call and it can on some hosts
                // be a considerable amount of routes.  This speeds up
                // the execution
                //
                // The variable signature is not completely decided and
                // must be adopted to what is appropriate
            }
            else if ("RemoveRoutes" == method_name)
            {
                // Similar to AddRoutes, receies an array of routes to
                // remove on this device
            }
            if ("AddDNS" == method_name)
            {
                if (!resolver)
                {
                    throw NetCfgException("No resolver configured");
                }

                // Adds DNS servers
                resolver->AddDNSServers(params);
            }
            else if ("RemoveDNS" == method_name)
            {
                if (!resolver)
                {
                    throw NetCfgException("No resolver configured");
                }

                // Removes DNS servers
                resolver->RemoveDNSServers(params);
            }
            else if ("AddDNSSearch" == method_name)
            {
                if (!resolver)
                {
                    throw NetCfgException("No resolver configured");
                }

                // Adds DNS search domains
                resolver->AddDNSSearch(params);
            }
            if ("RemoveDNSSearch" == method_name)
            {
                if (!resolver)
                {
                    throw NetCfgException("No resolver configured");
                }

                // Removes DNS search domains
                resolver->RemoveDNSSearch(params);
            }
            else if ("Establish" == method_name)
            {
                // This should generally be true for DBus 1.3, double checking here cannot hurt
                g_assert(g_dbus_connection_get_capabilities(conn) & G_DBUS_CAPABILITY_FLAGS_UNIX_FD_PASSING);

                // The virtual device has not yet been created on the host,
                // but all settings which has been queued up will be activated
                // when this method is called.
                if (resolver && resolver->GetModified())
                {
                    resolver->Apply();
                }
                int fd=-1; // TODO: return the real FD of the tun device here

                GUnixFDList *fdlist;
                GError *error;
                fdlist = g_unix_fd_list_new();
                g_unix_fd_list_append(fdlist, fd, &error);
                close(fd);

                if(error)
                {
                    throw NetCfgException("Creating fd list failed");
                }

                // DBus will close the handle on our side after transmitting
                g_dbus_method_invocation_return_value_with_unix_fd_list(invoc, nullptr, fdlist);
                GLibUtils::unref_fdlist(fdlist);

            }
            else if ("Disable" == method_name)
            {
                // This tears down and disables a virtual device but
                // enables the device to be re-activated again with the same
                // settings by calling the 'Activate' method again

                // Only restore the resolv.conf file if this is the last
                // device using these ResolverSettings object
                if (resolver && resolver->GetDeviceCount() <= 1)
                {
                    try
                    {
                        resolver->Restore();
                    }
                    catch (const NetCfgException& excp)
                    {
                        signal.LogCritical(excp.what());
                    }
                }
            }
            else if ("Destroy" == method_name)
            {
                // This should run 'Disable' if this has not happened
                // and then this object is completely deleted

                CheckOwnerAccess(sender);

                if (resolver)
                {
                    resolver->DecDeviceCount();
                    if (resolver->GetDeviceCount() == 0)
                    {
                        try
                        {
                            resolver->Restore();
                        }
                        catch (const NetCfgException& excp)
                        {
                            signal.LogCritical(excp.what());
                        }
                    }
                }

                std::string sender_name = lookup_username(GetUID(sender));
                signal.LogVerb1("Device '" + device_name + "' was removed by "
                               + sender_name);
                RemoveObject(conn);
                g_dbus_method_invocation_return_value(invoc, NULL);
                delete this;
                return;

            }
            g_dbus_method_invocation_return_value(invoc, retval);
            return;
        }
        catch (DBusCredentialsException& excp)
        {
            signal.LogCritical(excp.err());
            excp.SetDBusError(invoc);
        }
        catch (const std::exception& excp)
        {
            std::string errmsg = "Failed executing D-Bus call '" + method_name + "': " + excp.what();
            GError *err = g_dbus_error_new_for_dbus_error("net.openvpn.v3.netcfg.error.generic",
                                                          errmsg.c_str());
            g_dbus_method_invocation_return_gerror(invoc, err);
            g_error_free(err);
        }
        catch (...)
        {
            GError *err = g_dbus_error_new_for_dbus_error("net.openvpn.v3.netcfg.error.unspecified",
                                                          "Unknown error");
            g_dbus_method_invocation_return_gerror(invoc, err);
            g_error_free(err);
        }
    }


    /**
     *   Callback which is used each time a NetCfgServiceObject D-Bus property
     *   is being read.
     *
     * @param conn           D-Bus connection this event occurred on
     * @param sender         D-Bus bus name of the requester
     * @param obj_path       D-Bus object path to the object being requested
     * @param intf_name      D-Bus interface of the property being accessed
     * @param property_name  The property name being accessed
     * @param error          A GLib2 GError object if an error occurs
     *
     * @return  Returns a GVariant Glib2 object containing the value of the
     *          requested D-Bus object property.  On errors, NULL must be
     *          returned and the error must be returned via a GError
     *          object.
     */
    GVariant * callback_get_property(GDBusConnection *conn,
                                     const std::string sender,
                                     const std::string obj_path,
                                     const std::string intf_name,
                                     const std::string property_name,
                                     GError **error)
    {
        try
        {
            IdleCheck_UpdateTimestamp();
            validate_sender(sender);

            if ("log_level" == property_name)
            {
                return g_variant_new_uint32(signal.GetLogLevel());
            }
            else if ("owner" == property_name)
            {
                return GetOwner();
            }
            else if ("acl" == property_name)
            {
                return GetAccessList();
            }
            else if ("active" == property_name)
            {
                return g_variant_new_boolean(active);
            }
            else if ("modified" == property_name)
            {
                bool modified = false;
                if (resolver)
                {
                    modified |= resolver->GetModified();
                }
                return g_variant_new_boolean(modified);
            }
            else if ("ipv4_addresses" == property_name)
            {
                std::vector<std::string> iplist;
                // Popluate iplist, formatted as  "ipaddress/prefix"
                return GLibUtils::GVariantFromVector(iplist);
            }
            else if ("ipv4_routes" == property_name)
            {
                std::vector<std::string> routelist;
                // Popluate routelist, formatted as "ipaddress/prefix=>gw" ?
                return GLibUtils::GVariantFromVector(routelist);
            }
            else if ("ipv6_addresses" == property_name)
            {
                std::vector<std::string> iplist;
                // Popluate iplist, formatted as  "ipaddress/prefix"
                return GLibUtils::GVariantFromVector(iplist);
            }
            else if ("ipv6_routes" == property_name)
            {
                std::vector<std::string> routelist;
                // Popluate routelist, formatted as "ipaddress/prefix=>gw" ?
                return GLibUtils::GVariantFromVector(routelist);
            }
            else if ("dns_servers" == property_name)
            {
                if (!resolver)
                {
                    // If no resolver is configured, return an empty result
                    // instead of an error when reading this property
                    return GLibUtils::GVariantFromVector(std::vector<std::string>{});
                }
                return GLibUtils::GVariantFromVector(resolver->GetDNSServers());
            }
            else if (properties.Exists(property_name))
            {
                return properties.GetValue(property_name);
            }
        }
        catch (DBusPropertyException&)
        {
            throw;
        }
        catch (const NetCfgException & excp)
        {
            throw DBusPropertyException(G_IO_ERROR, G_IO_ERROR_FAILED,
                                        intf_name, obj_path, property_name,
                                        excp.what());
        }
        catch (const DBusException& excp)
        {
            throw DBusPropertyException(G_IO_ERROR, G_IO_ERROR_FAILED,
                                        intf_name, obj_path, property_name,
                                        excp.what());
        }
        throw DBusPropertyException(G_IO_ERROR, G_IO_ERROR_FAILED,
                                    obj_path, intf_name, property_name,
                                    "Invalid property");
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Unknown property");
        return NULL;
    }


    /**
     *  Callback method which is used each time a NetCfgServiceObject
     *  property is being modified over the D-Bus.
     *
     * @param conn           D-Bus connection this event occurred on
     * @param sender         D-Bus bus name of the requester
     * @param obj_path       D-Bus object path to the object being requested
     * @param intf_name      D-Bus interface of the property being accessed
     * @param property_name  The property name being accessed
     * @param value          GVariant object containing the value to be stored
     * @param error          A GLib2 GError object if an error occurs
     *
     * @return Returns a GVariantBuilder object containing the change
     *         confirmation on success.  On failures, an exception is thrown.
     *
     */
    GVariantBuilder * callback_set_property(GDBusConnection *conn,
                                            const std::string sender,
                                            const std::string obj_path,
                                            const std::string intf_name,
                                            const std::string property_name,
                                            GVariant *value,
                                            GError **error)
    {
        try
        {
            IdleCheck_UpdateTimestamp();
            validate_sender(sender);

            if ("log_level" == property_name)
            {
                unsigned int log_level = g_variant_get_uint32(value);
                if (log_level > 6)
                {
                    throw DBusPropertyException(G_IO_ERROR,
                                                G_IO_ERROR_INVALID_DATA,
                                                obj_path, intf_name,
                                                property_name,
                                                "Invalid log level");
                }
                signal.SetLogLevel(log_level);
                return build_set_property_response(property_name,
                                                   (guint32) log_level);
            }
        }
        catch (DBusPropertyException&)
        {
            throw;
        }
        catch (DBusException& excp)
        {
            throw DBusPropertyException(G_IO_ERROR, G_IO_ERROR_FAILED,
                                        obj_path, intf_name, property_name,
                                        excp.what());
        }
        throw DBusPropertyException(G_IO_ERROR, G_IO_ERROR_FAILED,
                                    obj_path, intf_name, property_name,
                                    "Invalid property");
    }




private:
    std::function<void()> remove_callback;

    // Properties
    PropertyCollection properties;
    NetCfgDeviceType device_type = NetCfgDeviceType::UNSET;
    std::string device_name;
    std::vector<std::string> dns_servers;
    std::vector<std::string> dns_search;
    unsigned int mtu;

    NetCfgSignals signal;
    DNS::ResolverSettings * resolver = nullptr;
    bool active = false;
    /**
     *  Validate that the sender is allowed to do change the configuration
     *  for this device.  If not, a DBusCredentialsException is thrown.
     *
     * @param sender  String containing the unique bus ID of the sender
     */
    void validate_sender(std::string sender)
    {
        return;  // FIXME: Currently disabled

        // Only the session manager is susposed to talk to the
        // the backend VPN client service
        if (GetUniqueBusID(OpenVPN3DBus_name_sessions) != sender)
        {
            throw DBusCredentialsException(GetUID(sender),
                                           "net.openvpn.v3.error.acl.denied",
                                           "You are not a session manager"
                                           );
        }
    }
};

