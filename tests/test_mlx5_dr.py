# SPDX-License-Identifier: (GPL-2.0 OR Linux-OpenIB)
# Copyright (c) 2020 Nvidia All rights reserved. See COPYING file
"""
Test module for pyverbs' mlx5 dr module.
"""

import unittest
import struct
import errno

from pyverbs.providers.mlx5.dr_action import DrActionQp, DrActionModify, \
    DrActionFlowCounter, DrActionDrop, DrActionTag, DrActionDestTable
from pyverbs.providers.mlx5.mlx5dv import Mlx5DevxObj, Mlx5Context, Mlx5DVContextAttr
from tests.utils import skip_unsupported, requires_root_on_eth, PacketConsts
from pyverbs.providers.mlx5.mlx5dv_flow import Mlx5FlowMatchParameters
from pyverbs.pyverbs_error import PyverbsRDMAError, PyverbsUserError
from pyverbs.providers.mlx5.dr_matcher import DrMatcher
from pyverbs.providers.mlx5.dr_domain import DrDomain
from pyverbs.providers.mlx5.dr_table import DrTable
from pyverbs.providers.mlx5.dr_rule import DrRule
import pyverbs.providers.mlx5.mlx5_enums as dve
from tests.mlx5_base import Mlx5RDMATestCase
from pyverbs.cq import CqInitAttrEx, CQEX
from tests.base import RawResources
import pyverbs.enums as e
import tests.utils as u

OUT_SMAC_47_16_FIELD_ID = 0x1
OUT_SMAC_47_16_FIELD_LENGTH = 32
OUT_SMAC_15_0_FIELD_ID = 0x2
OUT_SMAC_15_0_FIELD_LENGTH = 16
SET_ACTION = 0x1
MAX_MATCH_PARAM_SIZE = 0x180


class Mlx5DrResources(RawResources):
    """
    Test various functionalities of the mlx5 direct rules class.
    """
    def create_context(self):
        mlx5dv_attr = Mlx5DVContextAttr()
        try:
            self.ctx = Mlx5Context(mlx5dv_attr, name=self.dev_name)
        except PyverbsUserError as ex:
            raise unittest.SkipTest(f'Could not open mlx5 context ({ex})')
        except PyverbsRDMAError:
            raise unittest.SkipTest('Opening mlx5 context is not supported')

    def create_counter(self):
        """
        Create flow counter.
        :param player: The player to create the counter on.
        """
        from tests.mlx5_prm_structs import AllocFlowCounterIn, AllocFlowCounterOut
        self.counter = Mlx5DevxObj(self.ctx, AllocFlowCounterIn(), len(AllocFlowCounterOut()))
        self.flow_counter_id = AllocFlowCounterOut(self.counter.out_view).flow_counter_id

    def query_counter_packets(self):
        """
        Query flow counter packets count.
        :return: Number of packets on this counter.
        """
        from tests.mlx5_prm_structs import QueryFlowCounterIn, QueryFlowCounterOut
        query_in = QueryFlowCounterIn(flow_counter_id=self.flow_counter_id)
        counter_out = QueryFlowCounterOut(self.counter.query(query_in,
                                                             len(QueryFlowCounterOut())))
        return counter_out.flow_statistics.packets

    def __init__(self, dev_name, ib_port, gid_index, wc_flags=0):
        self.wc_flags = wc_flags
        super().__init__(dev_name=dev_name, ib_port=ib_port, gid_index=gid_index)

    @requires_root_on_eth()
    def create_qps(self):
        super().create_qps()

    def create_cq(self):
        """
        Create an Extended CQ.
        """
        wc_flags = e.IBV_WC_STANDARD_FLAGS | self.wc_flags
        cia = CqInitAttrEx(cqe=self.num_msgs, wc_flags=wc_flags)
        try:
            self.cq = CQEX(self.ctx, cia)
        except PyverbsRDMAError as ex:
            if ex.error_code == errno.EOPNOTSUPP:
                raise unittest.SkipTest('Create Extended CQ is not supported')
            raise ex


class Mlx5DrTest(Mlx5RDMATestCase):
    def setUp(self):
        super().setUp()
        self.iters = 10
        self.server = None
        self.client = None
        self.rules = []

    def tearDown(self):
        if self.server:
            self.server.ctx.close()
        if self.client:
            self.client.ctx.close()

    def create_players(self, resource, **resource_arg):
        """
        Init Dr test resources.
        :param resource: The RDMA resources to use.
        :param resource_arg: Dict of args that specify the resource specific
                             attributes.
        :return: None
        """
        self.client = resource(**self.dev_info, **resource_arg)
        self.server = resource(**self.dev_info, **resource_arg)

    @skip_unsupported
    def create_rx_recv_qp_rule(self, smac_value, actions):
        """
        Creates a rule on RX domain that forwards packets that match the smac in the matcher
        to the SW steering flow table and another rule on that table with provided actions.
        :param smac_value: The smac matcher value.
        :param actions: List of actions to attach to the recv rule.
        """
        self.domain_rx = DrDomain(self.server.ctx, dve.MLX5DV_DR_DOMAIN_TYPE_NIC_RX)
        root_table = DrTable(self.domain_rx, 0)
        table = DrTable(self.domain_rx, 1)
        smac_mask = bytes([0xff] * 6) + bytes(2)
        mask_param = Mlx5FlowMatchParameters(len(smac_mask), smac_mask)
        matcher = DrMatcher(table, 0, u.MatchCriteriaEnable.OUTER, mask_param)
        # Size of the matcher value should be modulo 4
        smac_value += bytes(2)
        value_param = Mlx5FlowMatchParameters(len(smac_value), smac_value)
        root_matcher = DrMatcher(root_table, 0, u.MatchCriteriaEnable.OUTER, mask_param)
        self.dest_table_action = DrActionDestTable(table)
        self.rules.append(DrRule(root_matcher, value_param, [self.dest_table_action]))
        self.rules.append(DrRule(matcher, value_param, actions))

    @skip_unsupported
    def create_tx_modify_rule(self):
        """
        Creares a rule on TX domain that modifies smac in the packet and sends
        it to the wire.
        """
        from tests.mlx5_prm_structs import SetActionIn
        self.domain_tx = DrDomain(self.client.ctx, dve.MLX5DV_DR_DOMAIN_TYPE_NIC_TX)
        table = DrTable(self.domain_tx, 0)
        smac_mask = bytes([0xff] * 6)
        mask_param = Mlx5FlowMatchParameters(len(smac_mask), smac_mask)
        matcher = DrMatcher(table, 0, u.MatchCriteriaEnable.OUTER, mask_param)
        smac_value = struct.pack('!6s', bytes.fromhex(PacketConsts.SRC_MAC.replace(':', '')))
        value_param = Mlx5FlowMatchParameters(len(smac_value), smac_value)
        action1 = SetActionIn(action_type=SET_ACTION, field=OUT_SMAC_47_16_FIELD_ID,
                              data=0x88888888, length=OUT_SMAC_47_16_FIELD_LENGTH)
        action2 = SetActionIn(action_type=SET_ACTION, field=OUT_SMAC_15_0_FIELD_ID,
                              data=0x8888, length=OUT_SMAC_15_0_FIELD_LENGTH)
        self.modify_actions = DrActionModify(self.domain_tx, dve.MLX5DV_DR_ACTION_FLAGS_ROOT_LEVEL,
                                             [action1, action2])
        self.rules.append(DrRule(matcher, value_param, [self.modify_actions]))

    @skip_unsupported
    def create_client_send_rule(self, actions):
        """
        Create rule over the client TX domain.
        :param actions: List of actions to attach to the send rule.
        """
        self.domain_tx = DrDomain(self.client.ctx, dve.MLX5DV_DR_DOMAIN_TYPE_NIC_TX)
        table = DrTable(self.domain_tx, 0)
        mask_param = Mlx5FlowMatchParameters(len(bytes([0xff] * 6)), bytes([0xff] * 6))
        matcher = DrMatcher(table, 0, u.MatchCriteriaEnable.OUTER, mask_param)
        smac_value = struct.pack('!6s', bytes.fromhex(PacketConsts.SRC_MAC.replace(':', '')))
        value_param = Mlx5FlowMatchParameters(len(smac_value), smac_value)
        self.rules.append(DrRule(matcher, value_param, actions))

    def send_client_raw_packets(self, iters, src_mac=None):
        """
        Send raw packets.
        :param iters: Number of packets to send.
        :param src_mac: If set, src mac to set in the packets.
        """
        c_send_wr, _, _ = u.get_send_elements_raw_qp(self.client, src_mac=src_mac)
        for _ in range(iters):
            u.send(self.client, c_send_wr, e.IBV_WR_SEND)
            u.poll_cq_ex(self.client.cq)

    @skip_unsupported
    def test_tbl_qp_rule(self):
        """
        Creates RX domain, SW table with matcher on source mac. Creates QP action
        and a rule with this action on the matcher.
        """
        self.create_players(Mlx5DrResources)
        self.qp_action = DrActionQp(self.server.qp)
        smac_value = struct.pack('!6s', bytes.fromhex(PacketConsts.SRC_MAC.replace(':', '')))
        self.create_rx_recv_qp_rule(smac_value, [self.qp_action])
        u.raw_traffic(self.client, self.server, self.iters)

    @skip_unsupported
    def test_tbl_modify_header_rule(self):
        """
        Creates TX domain, SW table with matcher on source mac and modify the smac.
        Then creates RX domain and rule that forwards packets with the new smac
        to server QP. Perform traffic that do this flow.
        """
        self.create_players(Mlx5DrResources)
        self.create_tx_modify_rule()
        src_mac = struct.pack('!6s', bytes.fromhex("88:88:88:88:88:88".replace(':', '')))
        self.qp_action = DrActionQp(self.server.qp)
        self.create_rx_recv_qp_rule(src_mac, [self.qp_action])
        exp_packet = u.gen_packet(self.client.msg_size, src_mac=src_mac)
        u.raw_traffic(self.client, self.server, self.iters, expected_packet=exp_packet)

    @skip_unsupported
    def test_tbl_counter_action(self):
        """
        Create flow counter object, attach it to a rule using counter action
        and perform traffic that hit this rule. Verify that the counter packets
        increased.
        """
        self.create_players(Mlx5DrResources)
        self.server.create_counter()
        self.server_counter_action = DrActionFlowCounter(self.server.counter)
        smac_value = struct.pack('!6s', bytes.fromhex(PacketConsts.SRC_MAC.replace(':', '')))
        self.qp_action = DrActionQp(self.server.qp)
        self.create_rx_recv_qp_rule(smac_value, [self.qp_action, self.server_counter_action])
        u.raw_traffic(self.client, self.server, self.iters)
        recv_packets = self.server.query_counter_packets()
        self.assertEqual(recv_packets, self.iters, 'Counter missed some recv packets')

    @skip_unsupported
    def test_prevent_duplicate_rule(self):
        """
        Creates RX domain, sets duplicate rule to be not allowed on that domain,
        try creating duplicate rule. Fail if creation succeeded.
        """
        from tests.mlx5_prm_structs import FlowTableEntryMatchParam

        self.server = Mlx5DrResources(**self.dev_info)
        domain_rx = DrDomain(self.server.ctx, dve.MLX5DV_DR_DOMAIN_TYPE_NIC_RX)
        domain_rx.allow_duplicate_rules(False)
        table = DrTable(domain_rx, 1)
        empty_param = Mlx5FlowMatchParameters(len(FlowTableEntryMatchParam()),
                                              FlowTableEntryMatchParam())
        matcher = DrMatcher(table, 0, u.MatchCriteriaEnable.NONE, empty_param)
        self.qp_action = DrActionQp(self.server.qp)
        self.drop_action = DrActionDrop()
        self.rules.append(DrRule(matcher, empty_param, [self.qp_action]))
        with self.assertRaises(PyverbsRDMAError) as ex:
            self.rules.append(DrRule(matcher, empty_param, [self.drop_action]))
            self.assertEqual(ex.exception.error_code, errno.EEXIST)

    @skip_unsupported
    def test_root_tbl_drop_action(self):
        """
        Create drop action on TX and verify using counter on the server RX that
        only packets that miss the drop rule arrived to the server RX.
        """
        self.create_players(Mlx5DrResources)
        # Create server counter.
        self.server.create_counter()
        self.server_counter_action = DrActionFlowCounter(self.server.counter)

        # Create rule that attaches all the packets in the server RX, sends them
        # to the server RX domain and counts them.
        domain_rx = DrDomain(self.server.ctx, dve.MLX5DV_DR_DOMAIN_TYPE_NIC_RX)
        table = DrTable(domain_rx, 0)
        mask_param = Mlx5FlowMatchParameters(MAX_MATCH_PARAM_SIZE, bytes(MAX_MATCH_PARAM_SIZE))
        matcher = DrMatcher(table, 0, u.MatchCriteriaEnable.NONE, mask_param)
        self.rx_drop_action = DrActionDrop()
        self.rules.append(DrRule(matcher, mask_param, [self.server_counter_action, self.rx_drop_action]))

        # Create drop action on the client TX on specific smac.
        self.tx_drop_action = DrActionDrop()
        self.create_client_send_rule([self.tx_drop_action])

        # Send packets with two differet smacs and expect half to be dropped.
        src_mac_drop = struct.pack('!6s', bytes.fromhex(PacketConsts.SRC_MAC.replace(':', '')))
        src_mac_non_drop = struct.pack('!6s', bytes.fromhex("88:88:88:88:88:88".replace(':', '')))
        self.send_client_raw_packets(int(self.iters/2), src_mac=src_mac_drop)
        recv_packets = self.server.query_counter_packets()
        self.assertEqual(recv_packets, 0, 'Drop action did not drop the TX packets')
        self.send_client_raw_packets(int(self.iters/2), src_mac=src_mac_non_drop)
        recv_packets = self.server.query_counter_packets()
        self.assertEqual(recv_packets, int(self.iters/2),
                         'Drop action dropped TX packets that not matched the rule')

    @skip_unsupported
    def test_tbl_qp_tag_rule(self):
        """
        Creates RX domain, table with matcher on source mac. Creates QP action
        and tag action. Creates a rule with those actions on the matcher.
        Verifies traffic and tag.
        """
        self.wc_flags = e.IBV_WC_EX_WITH_FLOW_TAG
        self.create_players(Mlx5DrResources,  wc_flags=e.IBV_WC_EX_WITH_FLOW_TAG)
        qp_action = DrActionQp(self.server.qp)
        tag = 0x123
        tag_action = DrActionTag(tag)
        smac_value = struct.pack('!6s', bytes.fromhex(PacketConsts.SRC_MAC.replace(':', '')))
        self.create_rx_recv_qp_rule(smac_value, [tag_action, qp_action])
        self.domain_rx.sync()
        u.raw_traffic(self.client, self.server, self.iters)
        # Verify tag
        self.assertEqual(self.server.cq.read_flow_tag(), tag, 'Wrong tag value')
