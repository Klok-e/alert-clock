import collections

import make_osm_fixture
import mock_server
import run_scenarios
from alert_gateway import basemap, geo, wire

KYIV = (50.4501, 30.5234)
RADIUS_KM = 30


def render_kyiv():
    """The basemap from a real Overpass response around central Kyiv (fixtures/osm)."""
    image = basemap.render(mock_server.OBLASTS, mock_server.RAIONS, {}, {}, make_osm_fixture.load("kyiv-30km"),
                           KYIV, RADIUS_KM, "м. Київ", run_scenarios.label_font())
    return [wire.decode_row(row) for row in image.rows]


def test_real_kyiv_data_keeps_water_roads_and_town_labels():
    # Dense real water once filled the 16 KB budget alone (now 20 KB), and roads and labels were dropped.
    pixels = collections.Counter(index for row in render_kyiv() for index in row)

    assert pixels[basemap.WATER] > 0
    assert pixels[basemap.ROAD] > 0
    assert pixels[basemap.LABEL] > 0


def test_a_reservoir_mapped_as_a_multipolygon_is_filled_not_outlined():
    # The Kyiv reservoir is a relation of 600+ member ways; a point well inside it must be water.
    x, y = geo.Projection(KYIV, RADIUS_KM).project_float((50.68, 30.48))

    assert render_kyiv()[round(y)][round(x)] == basemap.WATER


def test_water_never_hides_an_alert_fill():
    # Islands cut out of the reservoir once became bare ground over a red raion, and filled water
    # covered the alert colour; an alerted area must read as alerted wherever there is water.
    fills = {"київська": wire.LEVEL_RED, "м. київ": wire.LEVEL_RED}
    font = run_scenarios.label_font()

    def pixels(osm):
        image = basemap.render(mock_server.OBLASTS, mock_server.RAIONS, fills, {}, osm, KYIV, RADIUS_KM,
                               "м. Київ", font)
        return [index for row in image.rows for index in wire.decode_row(row)]

    without_osm = pixels(None)
    with_osm = pixels(make_osm_fixture.load("kyiv-30km"))
    lost = [i for i, (before, after) in enumerate(zip(without_osm, with_osm))
            if before == basemap.FILL_RED and after in (basemap.GROUND, basemap.WATER)]

    assert lost == []
