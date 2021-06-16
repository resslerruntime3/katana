"""
Discover and setup all Katana plugins.

"""

import importlib
import importlib.metadata
import logging
import sys
from collections import namedtuple
from importlib.abc import MetaPathFinder
from typing import Dict

logger = logging.getLogger(__name__)

installed_plugins = []


class PluginMetadata(namedtuple("PluginMetadata", ["name", "description", "version", "author", "licence"])):
    """Plugin metadata returned from a plugin."""

    # The human readable name.
    name: str

    # The prose description.
    description: str

    # The version of this plugin, ideally in a PEP-440 compatible format.
    # (https://www.python.org/dev/peps/pep-0440/#version-scheme)
    version: str

    # The author whether company or individual.
    author: str

    # The name of the license under which the plugin is distributed.
    licence: str


class KatanaPluginLoader(MetaPathFinder):
    _insert_table: Dict[str, str]

    def __init__(self, insert_table):
        super().__init__()
        self._insert_table = insert_table

    def find_module(self, fullname, path=None):
        if fullname in self._insert_table:
            logger.info(f"find_module {fullname} {path}")
            return self
        return None

    def load_module(self, fullname):
        if fullname in sys.modules:
            return sys.modules[fullname]
        logger.info(f"load_module {fullname}")

        assert fullname in self._insert_table

        # This will get the module from sys.modules if it is already loaded.
        module = importlib.import_module(self._insert_table[fullname])
        if fullname in sys.modules:
            raise ImportError(
                f"module {fullname} already exists while trying to insert "
                f"{self._insert_table[fullname]} at that name."
            )
        module.__name__ = fullname
        module.__package__ = fullname
        sys.modules[fullname] = module
        installed_plugins.append(
            getattr(module, "__katana_plugin_metadata__", PluginMetadata(fullname, None, None, None, None))
        )
        return module


def _register_plugin_loader():
    entry_points = importlib.metadata.entry_points()
    if "katana_plugin" in entry_points:
        katana_plugins = entry_points["katana_plugin"]
        table = {e.name: e.value for e in katana_plugins}
        sys.meta_path.append(KatanaPluginLoader(table))


_register_plugin_loader()
