"""The LOP nodes a session uses, in the /stage network by default.

    plasma_session       Sublayer LOP loading the session root, session.usda: the asset and every participant's layer
    plasma_mute_own      Configure Stage LOP muting this Houdini's participant layer (houdini-<user>.usda) in
                         Houdini's stage: the network, not that file, is the source of Houdini's opinions
    plasma_layer_break   Layer Break: nothing above it counts as Houdini's
    ...                  the artist's LOPs (Edit, Transform, Material Library, Configure Primitive, Prune, SOP Create..)
    plasma_live          Null with the display flag, the "Plasma Live" node: what the network authors between the
                         layer break and this node is Houdini's layer in the session

A fresh setup wires plasma_layer_break straight into plasma_live; edits go in between. When this Houdini's layer
already holds opinions from an earlier session (and no setup existed), they come back as an Inline USD LOP,
plasma_previous_edits, between the two, so joining does not drop them.

Each node carries user data naming its role and the session root, so joining again, also after reopening the .hip,
finds and reuses the nodes instead of building a second setup.
"""

import hou

from . import sessions

ROLE_KEY = "plasma_usd_live_role"
SESSION_KEY = "plasma_usd_live_session"

SESSION = "session"
MUTE = "mute"
BREAK = "break"
PREVIOUS = "previous"
LIVE = "live"

NODE_TYPES = {SESSION: "sublayer", MUTE: "configurestage", BREAK: "layerbreak", PREVIOUS: "inlineusd", LIVE: "null"}
NODE_NAMES = {SESSION: "plasma_session", MUTE: "plasma_mute_own", BREAK: "plasma_layer_break",
              PREVIOUS: "plasma_previous_edits", LIVE: "plasma_live"}
COLOR = hou.Color((0.35, 0.55, 0.95))


def lop_nodes_with_role():
    category = hou.lopNodeTypeCategory()
    for type_name in set(NODE_TYPES.values()):
        node_type = category.nodeTypes().get(type_name)
        if node_type is None:
            continue
        for node in node_type.instances():
            if node.userData(ROLE_KEY):
                yield node


def find(session_root):
    """{role: node} of the setup for a session already in the scene, from the network holding its plasma_live node
    (or any of its nodes); empty when there is none."""
    by_network = {}
    for node in lop_nodes_with_role():
        root = node.userData(SESSION_KEY)
        if root and sessions.same_path(root, session_root):
            by_network.setdefault(node.parent().path(), {})[node.userData(ROLE_KEY)] = node
    if not by_network:
        return {}
    return max(by_network.values(), key=lambda roles: (LIVE in roles, len(roles)))


def sessions_in_scene():
    """Session roots with a setup in the scene."""
    roots = {}
    for node in lop_nodes_with_role():
        root = node.userData(SESSION_KEY)
        if root:
            roots[sessions.path_key(root)] = root
    return list(roots.values())


def default_network():
    stage = hou.node("/stage")
    if stage is not None:
        return stage
    for node in hou.node("/").children():
        if node.childTypeCategory() == hou.lopNodeTypeCategory():
            return node
    raise RuntimeError("This scene has no LOP network (/stage) to build the session in")


def mute_pattern(path):
    return '"{}"'.format(path) if " " in path else path


class Setup:
    """The nodes of one session's setup, found again by session id so renaming or moving them does not matter."""

    def __init__(self, nodes):
        self.ids = {role: node.sessionId() for role, node in nodes.items()}

    def node(self, role):
        node_id = self.ids.get(role)
        node = hou.nodeBySessionId(node_id) if node_id is not None else None
        if node is None or node.userData(ROLE_KEY) != role:
            return None
        return node


def build(session_root, own_layer_path, session_name, participant, previous_usda=None):
    """Finds the session's setup in the scene, or builds it, and points it at the session. One undo step."""
    with hou.undos.group("Join Plasma USD Live session '{}'".format(session_name)):
        nodes = dict(find(session_root))
        fresh = LIVE not in nodes
        network = next(iter(nodes.values())).parent() if nodes else default_network()

        if not nodes:
            others = [c for c in network.children()]
            x = max((c.position()[0] for c in others), default=-4.0) + 4.0
            y = max((c.position()[1] for c in others), default=0.0)
            origin = hou.Vector2(x, y)
        else:
            origin = next(iter(nodes.values())).position()

        def make(role, input_node, offset):
            node = network.createNode(NODE_TYPES[role], NODE_NAMES[role])
            node.setUserData(ROLE_KEY, role)
            node.setUserData(SESSION_KEY, session_root)
            node.setColor(COLOR)
            if input_node is not None:
                node.setInput(0, input_node)
            node.setPosition(origin + hou.Vector2(0.0, -offset))
            nodes[role] = node
            return node

        if SESSION not in nodes:
            make(SESSION, None, 0.0)
        if MUTE not in nodes:
            make(MUTE, nodes[SESSION], 1.0)
        if BREAK not in nodes:
            make(BREAK, nodes[MUTE], 2.0)
        if fresh:
            upstream = nodes[BREAK]
            if previous_usda:
                upstream = make(PREVIOUS, upstream, 3.0)
                upstream.parm("usdsource").set(previous_usda)
                upstream.setComment("Opinions {} held when this setup was built; delete the node to drop them."
                                    .format(participant))
                upstream.setGenericFlag(hou.nodeFlag.DisplayComment, True)
            live = make(LIVE, upstream, 5.0)
            live.setDisplayFlag(True)

        session = nodes[SESSION]
        session.parm("filepath1").set(session_root)
        session.setComment("Plasma USD Live session '{}'".format(session_name))
        session.setGenericFlag(hou.nodeFlag.DisplayComment, True)

        mute = nodes[MUTE]
        mute.parm("editmute").set("addremove")
        mute.parm("mutepaths").set(mute_pattern(own_layer_path))
        mute.setComment("Mutes {}.usda: Houdini's opinions come from the network below".format(participant))
        mute.setGenericFlag(hou.nodeFlag.DisplayComment, True)

        live = nodes[LIVE]
        live.setComment("Plasma Live: edits wired in between plasma_layer_break and here go to the session as {}.usda"
                        .format(participant))
        live.setGenericFlag(hou.nodeFlag.DisplayComment, True)

    return Setup({role: node for role, node in nodes.items() if role != PREVIOUS}), fresh
