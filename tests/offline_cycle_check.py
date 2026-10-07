#!/usr/bin/env python3
import sys
import re

class WaitForGraph:
    def __init__(self):
        self.adj = {} # adjacency list: node -> set of neighbors

    def _ensure_node(self, node):
        if node not in self.adj:
            self.adj[node] = set()

    def add_edge(self, src, dst):
        self._ensure_node(src)
        self._ensure_node(dst)
        self.adj[src].add(dst)

    def remove_edge(self, src, dst):
        if src in self.adj and dst in self.adj[src]:
            self.adj[src].remove(dst)

    def process_event(self, event_type, tid, resource):
        """
        Build graph edges based on lock event types.
        WAIT: thread waits for resource (tid -> resource)
        HOLD: thread holds resource (resource -> tid)
        RELEASE: thread releases resource (remove resource -> tid)
        """
        if event_type == 'WAIT':
            self.add_edge(tid, resource)
        elif event_type == 'HOLD':
            self.remove_edge(tid, resource)  # Remove WAIT edge
            self.add_edge(resource, tid)
        elif event_type == 'RELEASE':
            self.remove_edge(resource, tid)
            # Remove any outstanding WAIT edge just in case
            self.remove_edge(tid, resource)

    def detect_cycle(self):
        """
        Detect a cycle using DFS with coloring (WHITE=0, GRAY=1, BLACK=2).
        Returns a list representing the cycle path if found, otherwise None.
        """
        color = {u: 0 for u in self.adj}
        parent = {}

        def dfs(u):
            color[u] = 1 # GRAY
            for v in self.adj.get(u, set()):
                if color.get(v, 0) == 0: # WHITE
                    parent[v] = u
                    cycle = dfs(v)
                    if cycle:
                        return cycle
                elif color.get(v, 0) == 1: # GRAY (cycle detected)
                    cycle = [v]
                    curr = u
                    while curr != v:
                        cycle.append(curr)
                        curr = parent[curr]
                    cycle.append(v)
                    cycle.reverse()
                    return cycle
            color[u] = 2 # BLACK
            return None

        for node in self.adj:
            if color[node] == 0:
                cycle = dfs(node)
                if cycle:
                    return cycle
        return None

def parse_log_file(filepath):
    graph = WaitForGraph()
    pattern = re.compile(r'^(WAIT|HOLD|RELEASE)\s+tid=(\S+)\s+resource=(\S+)')
    with open(filepath, 'r') as f:
        for line in f:
            match = pattern.search(line)
            if match:
                event_type, tid, resource = match.groups()
                graph.process_event(event_type, tid, resource)
    return graph.detect_cycle()

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python3 offline_cycle_check.py <logfile>")
        sys.exit(2)
        
    logfile = sys.argv[1]
    cycle = parse_log_file(logfile)
    
    if cycle:
        print(f"DEADLOCK DETECTED: {' -> '.join(cycle)}")
        sys.exit(1)
    else:
        print("No deadlock detected.")
        sys.exit(0)
