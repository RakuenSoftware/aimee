"""Hookspecs for the pluggy e2e fixture.

A pluggy plugin only IMPLEMENTS hooks; the host application defines them. That
is why aimee-pluggy-host.py requires --spec-module: without the specs there is
nothing to reflect into tools.
"""

import pluggy

hookspec = pluggy.HookspecMarker("aimee_demo")


@hookspec
def greet(name):
    """Return a greeting for name."""


@hookspec(firstresult=True)
def pick_one(options):
    """Return a single choice from options."""


@hookspec
def never_implemented(unused):
    """A hook no plugin implements; must not become a tool."""


@hookspec
def wrapped_only(value):
    """A hook implemented only by a wrapper; must not become a tool."""
