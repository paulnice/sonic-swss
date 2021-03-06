#include <string.h>
#include <errno.h>
#include <system_error>
#include <sys/socket.h>
#include <linux/if.h>
#include <netlink/route/link.h>
#include "logger.h"
#include "netmsg.h"
#include "dbconnector.h"
#include "producertable.h"
#include "tokenize.h"

#include "linkcache.h"
#include "portsyncd/linksync.h"

#include <iostream>
#include <set>

using namespace std;
using namespace swss;

#define VLAN_DRV_NAME   "bridge"
#define TEAM_DRV_NAME   "team"

extern set<string> g_portSet;
extern map<string, set<string>> g_vlanMap;
extern bool g_init;

LinkSync::LinkSync(DBConnector *db) :
    m_portTableProducer(db, APP_PORT_TABLE_NAME),
    m_vlanTableProducer(db, APP_VLAN_TABLE_NAME),
    m_lagTableProducer(db, APP_LAG_TABLE_NAME),
    m_portTableConsumer(db, APP_PORT_TABLE_NAME),
    m_vlanTableConsumer(db, APP_VLAN_TABLE_NAME),
    m_lagTableConsumer(db, APP_LAG_TABLE_NAME)
{
    /* See the comments for g_portSet in linksync.h */
    for (string port : g_portSet)
    {
        vector<FieldValueTuple> temp;
        if (m_portTableConsumer.get(port, temp))
        {
            for (auto it : temp)
            {
                if (fvField(it) == "admin_status")
                {
                    g_portSet.erase(port);
                    break;
                }
            }
        }
    }

    vector<KeyOpFieldsValuesTuple> tuples;
    m_vlanTableConsumer.getTableContent(tuples);

    for (auto tuple : tuples)
    {
        vector<string> keys = tokenize(kfvKey(tuple), ':');
        if (keys.size() != 2)
            continue;
        string vlan = keys[0];
        string member = keys[1];
        if (g_vlanMap.find(vlan) == g_vlanMap.end())
            g_vlanMap[vlan] = set<string>();
        g_vlanMap[vlan].insert(member);
    }
}

void LinkSync::onMsg(int nlmsg_type, struct nl_object *obj)
{
    if ((nlmsg_type != RTM_NEWLINK) && (nlmsg_type != RTM_DELLINK))
        return;

    struct rtnl_link *link = (struct rtnl_link *)obj;
    string key = rtnl_link_get_name(link);

    if (key == "lo" || key == "eth0" || key == "docker0" || key == "bcm0")
        return;

    unsigned int flags = rtnl_link_get_flags(link);
    bool admin = flags & IFF_UP;
    bool oper = flags & IFF_LOWER_UP;
    unsigned int mtu = rtnl_link_get_mtu(link);

    char addrStr[MAX_ADDR_SIZE+1] = {0};
    nl_addr2str(rtnl_link_get_addr(link), addrStr, MAX_ADDR_SIZE);

    unsigned int ifindex = rtnl_link_get_ifindex(link);
    int master = rtnl_link_get_master(link);
    char *type = rtnl_link_get_type(link);

    if (type)
        SWSS_LOG_DEBUG("nlmsg type:%d key:%s admin:%d oper:%d addr:%s ifindex:%d master:%d type:%s",
                       nlmsg_type, key.c_str(), admin, oper, addrStr, ifindex, master, type);
    else
        SWSS_LOG_DEBUG("nlmsg type:%d key:%s admin:%d oper:%d addr:%s ifindex:%d master:%d",
                       nlmsg_type, key.c_str(), admin, oper, addrStr, ifindex, master);

    /* Insert or update the ifindex to key map */
    m_ifindexNameMap[ifindex] = key;

    /* Will be dealt by teamsyncd */
    if (type && !strcmp(type, TEAM_DRV_NAME))
        return;

    /* VLAN member: A separate entry in VLAN_TABLE will be inserted */
    if (master)
    {
        string member_key = m_ifindexNameMap[master] + ":" + key;

        if (nlmsg_type == RTM_DELLINK) /* Will it happen? */
            m_vlanTableProducer.del(member_key);
        else /* RTM_NEWLINK */
        {
            vector<FieldValueTuple> fvVector;
            FieldValueTuple t("tagging_mode", "untagged");
            fvVector.push_back(t);

            m_vlanTableProducer.set(member_key, fvVector);
        }
    }
    /* No longer a VLAN member: Check if it was a member before and remove it */
    else
    {
        for (auto i = g_vlanMap.begin(); i != g_vlanMap.end(); i++)
        {
            set<string> member_set = (*i).second;
            if (member_set.find(key) != member_set.end())
            {
                string member_key = (*i).first + ":" + key;
                m_vlanTableProducer.del(member_key);
            }
        }
    }

    vector<FieldValueTuple> fvVector;
    FieldValueTuple a("admin_status", admin ? "up" : "down");
    FieldValueTuple o("oper_status", oper ? "up" : "down");
    FieldValueTuple m("mtu", to_string(mtu));
    fvVector.push_back(a);
    fvVector.push_back(o);
    fvVector.push_back(m);

    /* VLAN interfaces: Check if the type is bridge */
    if (type && !strcmp(type, VLAN_DRV_NAME))
    {
        if (nlmsg_type == RTM_DELLINK)
            m_vlanTableProducer.del(key);
        else
            m_vlanTableProducer.set(key, fvVector);

        return;
    }

    /* front panel interfaces: Check if the port is in the PORT_TABLE */
    vector<FieldValueTuple> temp;
    if (m_portTableConsumer.get(key, temp))
    {
        /* TODO: When port is removed from the kernel */
        if (nlmsg_type == RTM_DELLINK)
            return;

        if (!g_init && g_portSet.find(key) != g_portSet.end())
        {
            /* Bring up the front panel port as the first place*/
            system(("/sbin/ifup --force " + key).c_str());
            g_portSet.erase(key);
        }
        else
            m_portTableProducer.set(key, fvVector);

        return;
    }
}
