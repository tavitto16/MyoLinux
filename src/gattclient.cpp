/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gattclient.h"

#include <functional>
#include <sstream>

#define ARRAY_SIZEOF(array) (sizeof(array) / sizeof(array[0]))

namespace MYOLINUX_NAMESPACE {

void print_address(const GattClient::Address &address)
{
    std::ios state(nullptr);
    state.copyfmt(std::cout);

    for (std::size_t i = address.size(); i-- != 0; ) {
        std::cout << std::hex << std::setw(2) << static_cast<int>(address[i]);
        if (i != 0) {
            std::cout << ":";
        }
    }
    std::cout << std::endl;
    std::cout.copyfmt(state);
}

GattClient::GattClient(const Bled112Client &client)
    : client(client)
{ }

/// GattClient::discover
/// \param callback
void GattClient::discover(std::function<bool(std::int8_t, Address, Buffer)> callback)
{
    client.write(GapDiscover{GapDiscoverModeEnum::DiscoverGeneric});

    bool running = true;
    auto discover_response = [&](GapDiscoverResponse)
    { };

    auto discover_event = [&callback, &running](GapScanResponseEvent<0> event, Buffer data)
    {
        Address address;
        std::copy(event.sender, event.sender + ARRAY_SIZEOF(event.sender), std::begin(address));

        if (!callback(event.rssi, std::move(address), std::move(data))) {
            running = false;
        }
    };

    while (running) {
        client.read(discover_response, discover_event);
    }

    client.write(GapEndProcedure{});
    (void)client.read<GapEndProcedureResponse>();
}

/// GattClient::connect
/// \param address
void GattClient::connect(const Address &address)
{
    // Check if the connection already exists
    // Reviving the connection is only possible if no data has been sent i.e. setMode has not yet been called, otherwise
    // the device will disconnect automatically when the program exits. There will be a short window before the
    // disconnect in which the connection cannot be establised. To avoid this always call the disconnect method before
    // exiting the program or add sleep(1) before the connect call.
    for (std::uint8_t i = 0; i < 3; i++) { // The dongle supports 3 connections
        client.write(ConnectionGetStatus{i});
        (void)readResponse<ConnectionGetStatusResponse>();
        auto status = readResponse<ConnectionStatusEvent>();

        if (status.flags & ConnectionConnstatusEnum::Connected &&
                std::equal(std::begin(address), std::end(address), status.address)) {
            connection = i;
            return;
        }
    }

    GapConnectDirect command{{}, GapAddressTypeEnum::AddressTypePublic, 6, 6, 64, 0};
    std::copy(std::begin(address), std::end(address), command.address);
    client.write(command);

    const auto response = readResponse<GapConnectDirectResponse>();
    connection = response.connection_handle;

    (void)readResponse<ConnectionStatusEvent>();
    connected_ = true;
    address_ = address;
}

void GattClient::connect(const std::string &str)
{
    std::istringstream ss(str);
    Address address;


    for (std::size_t i = address.size(); i-- != 0; ) {
        int value;
        ss >> std::hex >> value;
        address[i] = static_cast<std::uint8_t>(value);

        char delimiter;
        ss >> delimiter;
        if (delimiter != ':') {
            throw std::runtime_error("Unexpected delimiter");
        }
    }

    connect(address);
}

bool GattClient::connected()
{
    return connected_;
}

auto GattClient::address() -> Address
{
    if (!connected_) {
        throw std::logic_error("Connection is not established, no address available.");
    }

    return address_;
}

void GattClient::disconnect(const std::uint8_t connection)
{
    client.write(ConnectionDisconnect{connection});
    (void)readResponse<ConnectionDisconnectResponse>();

    if (connected_ && this->connection == connection) {
        (void)readResponse<ConnectionDisconnectedEvent>();
        connected_ = false;
    }
}

void GattClient::disconnect()
{
    disconnect(connection);
}

void GattClient::disconnectAll()
{
    for (std::uint8_t i = 0; i < 3; i++) { // The dongle supports 3 connections
        disconnect(i);
    }
}

void GattClient::writeAttribute(const std::uint16_t handle, const Buffer &payload)
{
    client.write(AttclientAttributeWrite<0>{connection, handle, static_cast<std::uint8_t>(payload.size())}, payload);
    (void)readResponse<AttclientAttributeWriteResponse>();
    (void)readResponse<AttclientProcedureCompletedEvent>();
}

Buffer GattClient::readAttribute(const std::uint16_t handle)
{
    client.write(AttclientReadByHandle{connection, handle});
    (void)readResponse<AttclientReadByHandleResponse>();

    Buffer data;
retry:
    const auto metadata = client.read<AttclientAttributeValueEvent<0>>(data);
    if (metadata.atthandle != handle) {
        const auto handle = metadata.atthandle;
        event_queue.emplace_back(Event{handle, std::move(data)});
        goto retry;
    }

    if (metadata.length != data.size()) {
        throw std::runtime_error("Data length does not match the expected value.");
    }
    return data;
}

void GattClient::listen(const std::function<void(std::uint16_t, Buffer)> &callback)
{
    // The events get ofloaded to the queue when reading the read or write request response,
    // because the stream might have contained the events unrelated to the request.
    for (const auto &event : event_queue) {
        callback(std::get<0>(event), std::get<1>(event));
    }
    event_queue.clear();

    client.read([&callback](AttclientAttributeValueEvent<0> metadata, Buffer data) {
        callback(metadata.atthandle, std::move(data));
    });
}

auto GattClient::characteristics() -> Characteristics
{
    Characteristics chr;

    client.write(AttclientFindInformation{connection, 0x0001, 0xFFFF});
    (void)client.read<AttclientFindInformationResponse>();

    bool running = true;
    auto information_found = [&](AttclientFindInformationFoundEvent<0> event, Buffer uuid)
    {
        if (event.length != uuid.size()) {
            throw std::runtime_error("UUID size does not match the expected value.");
        }

        chr[uuid] = event.chrhandle;
    };

    auto procedure_completed = [&running](AttclientProcedureCompletedEvent)
    {
        running = false;
    };

    while(running) {
        client.read(information_found, procedure_completed);
    }

    return chr;
}

}
