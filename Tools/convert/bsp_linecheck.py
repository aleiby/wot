"""UE1 BSP point-region lookup and zero-extent solid-hull LineCheck.

The line traversal mirrors UModel::LineCheck's recursive zero-extent path.
It operates on either decode_model_full() output or zones.json's compact
parallel zone_bsp/leaf_bsp representation and has no Unreal dependency.
"""
from dataclasses import dataclass


NF_NOT_CSG = 0x01
NF_IS_NEW = 0x20


@dataclass(frozen=True)
class PointRegion:
    zone: int
    leaf: int


class BspTree:
    """Parallel UE1 model-node arrays used by point and line queries."""

    def __init__(self, *, planes, back_children, front_children,
                 back_leaves, front_leaves, node_csg, root_outside):
        self.planes = [tuple(float(value) for value in plane)
                       for plane in planes]
        self.back_children = [int(value) for value in back_children]
        self.front_children = [int(value) for value in front_children]
        self.back_leaves = [int(value) for value in back_leaves]
        self.front_leaves = [int(value) for value in front_leaves]
        self.node_csg = [bool(value) for value in node_csg]
        self.root_outside = bool(root_outside)
        count = len(self.planes)
        arrays = (self.back_children, self.front_children, self.back_leaves,
                  self.front_leaves, self.node_csg)
        if any(len(values) != count for values in arrays):
            raise ValueError("BSP parallel arrays have different lengths")
        if any(len(plane) != 4 for plane in self.planes):
            raise ValueError("BSP planes must contain four values")
        for children in (self.back_children, self.front_children):
            if any(child >= count for child in children):
                raise ValueError("BSP child index is outside the node array")

    @property
    def node_count(self):
        return len(self.planes)

    @classmethod
    def from_model(cls, bsp):
        """Build from lightmaps.decode_model_full() output."""
        back_children = []
        front_children = []
        for node_index, (back_child, front_child) in enumerate(
                bsp["node_children"]):
            back_zone, front_zone = bsp["nodes"][node_index][4]
            back_children.append(
                back_child if back_child >= 0 else -(int(back_zone) + 1))
            front_children.append(
                front_child if front_child >= 0 else -(int(front_zone) + 1))
        node_csg = [
            node[2] > 0 and not (flags & (NF_NOT_CSG | NF_IS_NEW))
            for node, flags in zip(bsp["nodes"], bsp["node_flags"])
        ]
        return cls(
            planes=bsp["node_planes"],
            back_children=back_children,
            front_children=front_children,
            back_leaves=[leaves[0] for leaves in bsp["node_leaves"]],
            front_leaves=[leaves[1] for leaves in bsp["node_leaves"]],
            node_csg=node_csg,
            root_outside=bsp["root_outside"],
        )

    @classmethod
    def from_export(cls, data):
        """Build from zones.json's zone_bsp and leaf_bsp sections."""
        zone_nodes = data.get("zone_bsp", [])
        leaf_bsp = data.get("leaf_bsp", {})
        leaf_nodes = leaf_bsp.get("nodes", [])
        if len(zone_nodes) != len(leaf_nodes):
            raise ValueError("zone_bsp and leaf_bsp node counts differ")
        planes = []
        back_children = []
        front_children = []
        for row in zone_nodes:
            if isinstance(row, dict):
                planes.append(row["plane"])
                back_children.append(row["back"])
                front_children.append(row["front"])
            else:
                planes.append(row[0:4])
                front_children.append(row[4])
                back_children.append(row[5])
        return cls(
            planes=planes,
            back_children=back_children,
            front_children=front_children,
            back_leaves=[row[0] for row in leaf_nodes],
            front_leaves=[row[1] for row in leaf_nodes],
            node_csg=[row[2] for row in leaf_nodes],
            root_outside=leaf_bsp.get("root_outside", False),
        )

    def _distance(self, node_index, point):
        x, y, z = (float(point[0]), float(point[1]), float(point[2]))
        plane = self.planes[node_index]
        return x * plane[0] + y * plane[1] + z * plane[2] - plane[3]

    def point_region(self, point):
        """Return compiled zone and leaf; coplanar points choose front."""
        if not self.planes:
            return PointRegion(zone=0, leaf=-1)
        node_index = 0
        for _step in range(self.node_count):
            if not 0 <= node_index < self.node_count:
                return PointRegion(zone=0, leaf=-1)
            side = 1 if self._distance(node_index, point) >= 0.0 else 0
            child = (self.front_children[node_index] if side
                     else self.back_children[node_index])
            if child < 0:
                leaf = (self.front_leaves[node_index] if side
                        else self.back_leaves[node_index])
                return PointRegion(zone=-child - 1, leaf=leaf)
            node_index = child
        return PointRegion(zone=0, leaf=-1)

    def _child_outside(self, node_index, side, outside):
        is_csg = self.node_csg[node_index]
        return (outside or is_csg) if side else (outside and not is_csg)

    def line_check(self, start, end):
        """Return True when Start->End stays outside the solid BSP hull."""
        if not self.planes:
            return self.root_outside
        budget = [self.node_count * 4 + 1]
        return self._line_check_node(
            0, tuple(end), tuple(start), self.root_outside, budget)

    def _line_check_node(self, node_index, end, start, outside, budget):
        while node_index >= 0:
            budget[0] -= 1
            if budget[0] < 0 or node_index >= self.node_count:
                return False
            distance_start = self._distance(node_index, start)
            distance_end = self._distance(node_index, end)
            if distance_start > -0.001 and distance_end > -0.001:
                outside = outside or self.node_csg[node_index]
                node_index = self.front_children[node_index]
            elif distance_start < 0.001 and distance_end < 0.001:
                outside = outside and not self.node_csg[node_index]
                node_index = self.back_children[node_index]
            else:
                alpha = distance_start / (distance_end - distance_start)
                middle = tuple(
                    start[axis] + (start[axis] - end[axis]) * alpha
                    for axis in range(3)
                )
                front_first = 1 if distance_start > 0.0 else 0
                first_child = (self.front_children[node_index] if front_first
                               else self.back_children[node_index])
                if not self._line_check_node(
                        first_child, middle, start,
                        self._child_outside(
                            node_index, front_first, outside), budget):
                    return False
                other_side = 1 - front_first
                outside = self._child_outside(
                    node_index, other_side, outside)
                node_index = (self.front_children[node_index] if other_side
                              else self.back_children[node_index])
                start = middle
        return bool(outside)
