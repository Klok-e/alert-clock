import make_wire_fixtures


def test_the_clock_decoder_fixtures_match_the_current_encoder():
    # The C++ tests decode these bytes; a protocol change on one side must fail a test
    # until `python3 tools/make_wire_fixtures.py` brings both sides back together.
    assert make_wire_fixtures.OUTPUT.read_text() == make_wire_fixtures.render(), \
        "test/wire_fixtures.h is out of date: run python3 tools/make_wire_fixtures.py"
