# -*- coding: utf-8 -*-
import sys
import os
import urllib.parse

project_root = os.path.dirname(os.path.abspath(__file__))
lib_path = os.path.join(project_root, 'resources', 'lib')
if lib_path not in sys.path:
    sys.path.insert(0, lib_path)

from api_client import ApiClient
from vfs_adapter import VfsAdapter
from router import VfsRouter

def main():
    from daemon import ensure_daemon_started
    ensure_daemon_started()

    handle = int(sys.argv[1])
    query_string = sys.argv[2] if len(sys.argv) > 2 else ""

    params = dict(urllib.parse.parse_qsl(query_string.lstrip('?')))
    search_term = params.get('q', '')

    if not search_term:
        return

    api_client = ApiClient()
    adapter = VfsAdapter(handle, sys.argv[0])
    router = VfsRouter(adapter, api_client)
    router.show_search_results(search_term, page=1)

if __name__ == '__main__':
    main()