"""A real pluggy plugin for the e2e fixture.

Covers the three cases the host has to distinguish:
  * a plain hook           -> a tool returning the LIST of results
  * a firstresult hook     -> a tool returning the single result
  * a wrapper-only hook    -> NOT a tool (a wrapper is not a callable surface)
"""

import pluggy

hookimpl = pluggy.HookimplMarker("aimee_demo")


@hookimpl
def greet(name):
    return f"hello {name}"


@hookimpl
def pick_one(options):
    return options[0] if options else None


@hookimpl(hookwrapper=True)
def wrapped_only(value):
    outcome = yield
    return outcome
