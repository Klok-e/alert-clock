from alert_gateway import neptun


def make_neptun():
    clock = neptun.Clock()
    clock.set(1_000_000.0)
    return neptun.Neptun(config=None, clock=clock, on_change=lambda: None), clock


def test_heartbeats_keep_a_source_fresh_only_while_its_rest_fetch_succeeds():
    # NEPTUN's stream sends heartbeats even when its data has stopped updating. A heartbeat
    # vouches for a source only after that source's latest REST fetch worked.
    feed, clock = make_neptun()
    feed.handle_frame('{"type":"alerts","data":{"raions":[],"oblasts":[]}}')
    feed.sources["alerts"].rest_ok = True
    feed.sources["threats"].rest_ok = False  # its REST fetch failed

    clock.set(clock.now() + 200)
    feed.handle_frame('{"type":"heartbeat"}')

    assert not feed.sources["alerts"].stale(clock.now())
    assert feed.sources["threats"].stale(clock.now())
