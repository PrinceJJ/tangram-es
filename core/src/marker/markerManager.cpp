#include "data/tileData.h"
#include "marker/markerManager.h"
#include "marker/marker.h"
#include "scene/sceneLoader.h"
#include "style/style.h"

namespace Tangram {

void MarkerManager::setScene(std::shared_ptr<Scene> scene) {

    m_scene = scene;
    m_mapProjection = scene->mapProjection().get();
    m_styleContext.initFunctions(*scene);
    m_jsFnIndex = scene->functions().size();

    // FIXME: Styling data stored in the scene, like 'stops', will get trashed when a new scene is loaded!

    // Initialize StyleBuilders
    for (auto& style : scene->styles()) {
        m_styleBuilders[style->getName()] = style->createBuilder();
    }

}

Marker* MarkerManager::add(const char* styling) {

    // Add a new empty marker object to the list of markers.
    m_markers.push_back(std::make_unique<Marker>());

    // Create a draw rule from the given styling string.
    auto marker = m_markers.back().get();
    setStyling(marker, styling);

    // Return a pointer to the marker.
    return marker;

}

bool MarkerManager::remove(Marker* marker) {
    for (auto it = m_markers.begin(), end = m_markers.end(); it != end; ++it) {
        if (it->get() == marker) {
            m_markers.erase(it);
            return true;
        }
    }
    return false;
}

bool MarkerManager::setStyling(Marker* marker, const char* styling) {
    if (!marker || !contains(marker)) { return false; }

    // Update the draw rule for the marker.
    YAML::Node node = YAML::Load(styling);
    std::vector<StyleParam> params;
    SceneLoader::parseStyleParams(node, m_scene, "", params);

    // Compile any new JS functions used for styling.
    const auto& sceneJsFnList = m_scene->functions();
    for (auto i = m_jsFnIndex; i < sceneJsFnList.size(); ++i) {
        m_styleContext.addFunction(sceneJsFnList[i]);
    }
    m_jsFnIndex = sceneJsFnList.size();

    marker->setStyling(std::make_unique<DrawRuleData>("", 0, std::move(params)));

    // Build the feature mesh for the marker's current geometry.
    build(*marker, m_zoom);
    return true;
}

bool MarkerManager::setPoint(Marker* marker, LngLat lngLat) {
    if (!marker || !contains(marker)) { return false; }

    // If the marker does not have a 'point' feature mesh built, build it.
    if (!marker->mesh() || !marker->feature() || marker->feature()->geometryType != GeometryType::points) {
        auto feature = std::make_unique<Feature>();
        feature->geometryType = GeometryType::points;
        feature->points.emplace_back();
        marker->setFeature(std::move(feature));
        build(*marker, m_zoom);
    }

    // Update the marker's bounds to the given coordinates.
    auto origin = m_mapProjection->LonLatToMeters({ lngLat.longitude, lngLat.latitude });
    marker->setBounds({ origin, origin });

    return true;
}

bool MarkerManager::setPointEased(Marker* marker, LngLat lngLat, float duration, EaseType ease) {
    if (!marker || !contains(marker)) { return false; }

    // If the marker does not have a 'point' feature built, we can't ease it.
    if (!marker->mesh() || !marker->feature() || marker->feature()->geometryType != GeometryType::points) {
        return false;
    }

    auto dest = m_mapProjection->LonLatToMeters({ lngLat.longitude, lngLat.latitude });
    marker->setEase(dest, duration, ease);

    return true;
}

bool MarkerManager::setPolyline(Marker* marker, LngLat* coordinates, int count) {
    if (!marker || !contains(marker)) { return false; }
    if (!coordinates || count < 2) { return false; }

    // Build a feature for the new set of polyline points.
    auto feature = std::make_unique<Feature>();
    feature->geometryType = GeometryType::lines;
    feature->lines.emplace_back();
    auto& line = feature->lines.back();

    // Determine the bounds of the polyline.
    BoundingBox bounds;
    bounds.min = { coordinates[0].longitude, coordinates[0].latitude };
    bounds.max = bounds.min;
    for (int i = 0; i < count; ++i) {
        bounds.expand(coordinates[i].longitude, coordinates[i].latitude);
    }
    bounds.min = m_mapProjection->LonLatToMeters(bounds.min);
    bounds.max = m_mapProjection->LonLatToMeters(bounds.max);

    // Update the marker's bounds.
    marker->setBounds(bounds);

    float scale = 1.f / marker->extent();

    // Project and offset the coordinates into the marker-local coordinate system.
    auto origin = marker->origin(); // SW corner.
    for (int i = 0; i < count; ++i) {
        auto degrees = glm::dvec2(coordinates[i].longitude, coordinates[i].latitude);
        auto meters = m_mapProjection->LonLatToMeters(degrees);
        line.emplace_back((meters.x - origin.x) * scale, (meters.y - origin.y) * scale, 0.f);
    }

    // Update the feature data for the marker.
    marker->setFeature(std::move(feature));

    // Build a new mesh for the marker.
    build(*marker, m_zoom);

    return true;
}

bool MarkerManager::setPolygon(Marker* marker, LngLat* coordinates, int* counts, int rings) {
    if (!marker || !contains(marker)) { return false; }
    if (!coordinates || !counts || rings < 1) { return false; }

    // Build a feature for the new set of polygon points.
    auto feature = std::make_unique<Feature>();
    feature->geometryType = GeometryType::polygons;
    feature->polygons.emplace_back();
    auto& polygon = feature->polygons.back();

    // Determine the bounds of the polygon.
    BoundingBox bounds;
    LngLat* ring = coordinates;
    for (int i = 0; i < rings; ++i) {
        int count = counts[i];
        for (int j = 0; j < count; ++j) {
            if (i == 0 && j == 0) {
                bounds.min = { ring[0].longitude, ring[0].latitude };
                bounds.max = bounds.min;
            }
            bounds.expand(ring[j].longitude, ring[j].latitude);
        }
        ring += count;
    }
    bounds.min = m_mapProjection->LonLatToMeters(bounds.min);
    bounds.max = m_mapProjection->LonLatToMeters(bounds.max);

    // Update the marker's bounds.
    marker->setBounds(bounds);

    float scale = 1.f / marker->extent();

    // Project and offset the coordinates into the marker-local coordinate system.
    auto origin = marker->origin(); // SW corner.
    ring = coordinates;
    for (int i = 0; i < rings; ++i) {
        int count = counts[i];
        polygon.emplace_back();
        auto& line = polygon.back();
        for (int j = 0; j < count; ++j) {
            auto degrees = glm::dvec2(ring[j].longitude, ring[j].latitude);
            auto meters = m_mapProjection->LonLatToMeters(degrees);
            line.emplace_back((meters.x - origin.x) * scale, (meters.y - origin.y) * scale, 0.f);
        }
        ring += count;
    }

    // Update the feature data for the marker.
    marker->setFeature(std::move(feature));

    // Build a new mesh for the marker.
    build(*marker, m_zoom);

    return true;
}

bool MarkerManager::update(int zoom) {

    if (zoom == m_zoom) {
         return false;
    }
    bool rebuilt = false;
    for (auto& marker : m_markers) {
        if (zoom != marker->builtZoomLevel()) {
            build(*marker, zoom);
            rebuilt = true;
        }
    }
    m_zoom = zoom;
    return rebuilt;
}

void MarkerManager::removeAll() {

    m_markers.clear();

}

const std::vector<std::unique_ptr<Marker>>& MarkerManager::markers() const {
    return m_markers;
}

void MarkerManager::build(Marker& marker, int zoom) {

    auto rule = marker.drawRule();
    auto feature = marker.feature();

    if (!rule || !feature) { return; }

    StyleBuilder* styler = nullptr;
    {
        auto name = rule->getStyleName();
        auto it = m_styleBuilders.find(name);
        if (it != m_styleBuilders.end()) {
            styler = it->second.get();
        } else {
            LOGN("Invalid style %s", name.c_str());
            return;
        }
    }

    m_styleContext.setKeywordZoom(zoom);

    bool valid = m_ruleSet.evaluateRuleForContext(*rule, m_styleContext);

    if (valid) {
        styler->setup(marker, zoom);
        styler->addFeature(*feature, *rule);
        marker.setMesh(styler->style().getID(), zoom, styler->build());
    }

}

bool MarkerManager::contains(Marker* marker) {
    for (auto it = m_markers.begin(), end = m_markers.end(); it != end; ++it) {
        if (it->get() == marker) { return true; }
    }
    return false;
}

} // namespace Tangram
