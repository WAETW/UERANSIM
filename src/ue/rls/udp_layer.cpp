#include "udp_layer.hpp"

#include <cstdint>
#include <set>

#include <ue/l3/task.hpp>
#include <ue/nts.hpp>
#include <ue/rls/task.hpp>
#include <utils/common.hpp>
#include <utils/constants.hpp>

static constexpr const int LOOP_PERIOD = 1000;
static constexpr const int HEARTBEAT_THRESHOLD = 2000; // (LOOP_PERIOD + RECEIVE_TIMEOUT)'dan büyük olmalı

namespace nr::ue
{

RlsUdpLayer::RlsUdpLayer(TaskBase *base)
    : m_base{base}, m_server{}, m_searchSpace{}, m_cells{}, m_cellIdToSti{}, m_lastLoop{}, m_cellIdCounter{}
{
    m_logger = base->logBase->makeUniqueLogger(base->config->getLoggerPrefix() + "rls-udp");

    for (auto &ip : base->config->gnbSearchList)
        m_searchSpace.emplace_back(ip, cons::RadioLinkPort);

    m_simPos = Vector3{};
}

void RlsUdpLayer::onStart()
{
    m_server = std::make_unique<udp::UdpServerTask>(m_base->rlsTask);
    m_server->start();
}

void RlsUdpLayer::checkHeartbeat()
{
    auto current = utils::CurrentTimeMillis();
    if (current - m_lastLoop > LOOP_PERIOD)
    {
        m_lastLoop = current;
        heartbeatCycle(current, m_simPos);
    }
}

void RlsUdpLayer::onQuit()
{
    m_server->quit();
}

void RlsUdpLayer::sendRlsPdu(const InetAddress &address, const rls::RlsMessage &msg)
{
    OctetString stream;
    rls::EncodeRlsMessage(msg, stream);

    m_server->send(address, stream);
}

void RlsUdpLayer::send(int cellId, const rls::RlsMessage &msg)
{
    if (m_cellIdToSti.count(cellId))
    {
        auto sti = m_cellIdToSti[cellId];
        sendRlsPdu(m_cells[sti].address, msg);
    }
}

void RlsUdpLayer::receiveRlsPdu(const InetAddress &addr, std::unique_ptr<rls::RlsMessage> &&msg)
{
    if (msg->msgType == rls::EMessageType::HEARTBEAT_ACK)
    {
        if (!m_cells.count(msg->sti))
        {
            m_cells[msg->sti].cellId = ++m_cellIdCounter;
            m_cellIdToSti[m_cells[msg->sti].cellId] = msg->sti;
        }

        int oldDbm = INT32_MIN;
        if (m_cells.count(msg->sti))
            oldDbm = m_cells[msg->sti].dbm;

        m_cells[msg->sti].address = addr;
        m_cells[msg->sti].lastSeen = utils::CurrentTimeMillis();

        int newDbm = ((const rls::RlsHeartBeatAck &)*msg).dbm;
        m_cells[msg->sti].dbm = newDbm;

        if (oldDbm != newDbm)
            onSignalChangeOrLost(m_cells[msg->sti].cellId);
        return;
    }

    if (!m_cells.count(msg->sti))
    {
        // if no HB-ACK received yet, and the message is not HB-ACK, then ignore the message
        return;
    }

    m_base->rlsTask->ctl().handleRlsMessage(m_cells[msg->sti].cellId, *msg);
}

void RlsUdpLayer::onSignalChangeOrLost(int cellId)
{
    int dbm = INT32_MIN;
    if (m_cellIdToSti.count(cellId))
    {
        auto sti = m_cellIdToSti[cellId];
        dbm = m_cells[sti].dbm;
    }

    auto m = std::make_unique<NmUeRlsToRrc>(NmUeRlsToRrc::SIGNAL_CHANGED);
    m->cellId = cellId;
    m->dbm = dbm;
    m_base->l3Task->push(std::move(m));
}

void RlsUdpLayer::heartbeatCycle(uint64_t time, const Vector3 &simPos)
{
    std::set<std::pair<uint64_t, int>> toRemove;

    for (auto &cell : m_cells)
    {
        auto delta = time - cell.second.lastSeen;
        if (delta > HEARTBEAT_THRESHOLD)
            toRemove.insert({cell.first, cell.second.cellId});
    }

    for (auto cell : toRemove)
    {
        m_cells.erase(cell.first);
        m_cellIdToSti.erase(cell.second);
    }

    for (auto cell : toRemove)
        onSignalChangeOrLost(cell.second);

    for (auto &address : m_searchSpace)
    {
        rls::RlsHeartBeat msg{m_base->shCtx.sti};
        msg.simPos = simPos;
        sendRlsPdu(address, msg);
    }
}

} // namespace nr::ue
