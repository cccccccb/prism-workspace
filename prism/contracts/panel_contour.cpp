#include "prism/contracts/panel_contour.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <variant>

namespace prism::contracts {
namespace {
constexpr double CornerControl = 0.5522847498307936;

void ValidateBody(LogicalSize body)
{
    if (!std::isfinite(body.width) || !std::isfinite(body.height) || body.width <= 0 ||
        body.height <= 0 || body.width > ContourCoordinateLimit ||
        body.height > ContourCoordinateLimit) {
        throw std::invalid_argument("Panel contour body must be positive and at most 8192");
    }
}

double Radius(LogicalSize body, const PanelContourSpec &spec)
{
    return std::min({spec.radius, body.width / 2, body.height / 2});
}

double NeckHeight(const PanelContourSpec &spec)
{
    return spec.neck_width == 0 || spec.neck_height == 0 ? 0 : spec.neck_height;
}

std::pair<double, double> AttachmentLimits(LogicalSize body, const PanelContourSpec &spec)
{
    const double inset = Radius(body, spec) + spec.neck_width / 2;
    return {inset, body.width - inset};
}

void Line(ContourPath &path, LogicalPoint end)
{
    path.segments.emplace_back(ContourLine{end});
}

void Corner(ContourPath &path, LogicalPoint control1, LogicalPoint control2, LogicalPoint end,
            double radius)
{
    if (radius == 0) {
        Line(path, end);
        return;
    }
    path.segments.emplace_back(ContourCubic{control1, control2, end});
}

void SoftTabNeck(ContourPath &path, const PanelContourRequest &request, double radius,
                 double height)
{
    const double half = request.spec.neck_width / 2;
    const double left = request.center - half;
    const double right = request.center + half;
    Line(path, {left, 0});
    if (radius == 0) {
        Line(path, {left, -height});
        Line(path, {right, -height});
        Line(path, {right, 0});
        return;
    }

    // The full footprint includes both shoulders. Monotone S-curves have
    // horizontal tangents at the body and the rounded horizontal tip, without
    // crossing or expanding outside [left,right] x [-height,0].
    const double tip_half = half / 3;
    const double shoulder = half - tip_half;
    const double left_tip = request.center - tip_half;
    const double right_tip = request.center + tip_half;
    path.segments.emplace_back(ContourCubic{
        {left + shoulder / 2, 0}, {left_tip - shoulder / 2, -height}, {left_tip, -height}});
    Line(path, {right_tip, -height});
    path.segments.emplace_back(
        ContourCubic{{right_tip + shoulder / 2, -height}, {right - shoulder / 2, 0}, {right, 0}});
}

LogicalPoint ReflectNeckPoint(LogicalPoint point, double center)
{
    return {2 * center - point.x, point.y};
}

void AppendReflectedNeckCurve(ContourPath &path, const ContourCubic &curve, LogicalPoint start,
                              double center)
{
    path.segments.emplace_back(ContourCubic{ReflectNeckPoint(curve.control2, center),
                                            ReflectNeckPoint(curve.control1, center),
                                            ReflectNeckPoint(start, center)});
}

void RoundedTriangleNeck(ContourPath &path, const PanelContourRequest &request, double radius,
                         double height)
{
    const double half = request.spec.neck_width / 2;
    const double center = request.center;
    const LogicalPoint left{center - half, 0};
    const LogicalPoint apex{center, -height};
    Line(path, left);
    if (radius == 0) {
        Line(path, apex);
        Line(path, {center + half, 0});
        return;
    }

    // Each side is two monotone curves: the broad shoulder leads directly
    // into the rounded tip without a straight diagonal. Their matching join
    // handles give equal derivatives; mirrored apex handles remain horizontal.
    const double handle = std::min(radius / 3, half / 4);
    const LogicalPoint join{center - 2 * handle, -.65 * height};
    const LogicalPoint tangent{handle / 2, -.2 * height};
    const ContourCubic body{
        {left.x + .35 * half, 0}, {join.x - tangent.x, join.y - tangent.y}, join};
    const ContourCubic tip{
        {join.x + tangent.x, join.y + tangent.y}, {apex.x - handle, apex.y}, apex};
    path.segments.emplace_back(body);
    path.segments.emplace_back(tip);
    AppendReflectedNeckCurve(path, tip, join, center);
    AppendReflectedNeckCurve(path, body, left, center);
}

ContourPath BodyPath(const PanelContourRequest &request, double height)
{
    const double width = request.body.width;
    const double bottom = request.body.height;
    const double radius = Radius(request.body, request.spec);
    const double control = CornerControl * radius;
    ContourPath path{{radius, 0}, {}};
    if (height > 0) {
        if (request.spec.neck_shape == PanelNeckShape::RoundedTriangle) {
            RoundedTriangleNeck(path, request, radius, height);
        } else {
            SoftTabNeck(path, request, radius, height);
        }
    }
    Line(path, {width - radius, 0});
    Corner(path, {width - radius + control, 0}, {width, radius - control}, {width, radius}, radius);
    Line(path, {width, bottom - radius});
    Corner(path, {width, bottom - radius + control}, {width - radius + control, bottom},
           {width - radius, bottom}, radius);
    Line(path, {radius, bottom});
    Corner(path, {radius - control, bottom}, {0, bottom - radius + control}, {0, bottom - radius},
           radius);
    Line(path, {0, radius});
    Corner(path, {0, radius - control}, {radius - control, 0}, {radius, 0}, radius);
    return path;
}

void MirrorPoint(LogicalPoint &point, double height)
{
    point.y = height - point.y;
}

void MirrorPath(ContourPath &path, double height)
{
    MirrorPoint(path.start, height);
    for (auto &segment : path.segments) {
        if (auto *line = std::get_if<ContourLine>(&segment)) {
            MirrorPoint(line->end, height);
        } else {
            auto &curve = std::get<ContourCubic>(segment);
            MirrorPoint(curve.control1, height);
            MirrorPoint(curve.control2, height);
            MirrorPoint(curve.end, height);
        }
    }
}

double ValidateRequest(const PanelContourRequest &request)
{
    ValidateBody(request.body);
    ValidatePanelContourSpec(request.spec);
    if (!std::isfinite(request.center)) {
        throw std::invalid_argument("Panel contour center must be finite");
    }
    switch (request.edge) {
    case PanelAttachmentEdge::None:
        return 0;
    case PanelAttachmentEdge::Top:
    case PanelAttachmentEdge::Bottom:
        break;
    default:
        throw std::invalid_argument("Unknown panel attachment edge");
    }

    const double height = NeckHeight(request.spec);
    if (height == 0) {
        return 0;
    }
    const auto [left, right] = AttachmentLimits(request.body, request.spec);
    if (left > right || request.center < left || request.center > right) {
        throw std::invalid_argument("Panel neck footprint does not fit the body edge");
    }
    if (request.body.height + height > ContourCoordinateLimit) {
        throw std::invalid_argument("Panel and neck exceed the contour height limit");
    }
    return height;
}
} // namespace

void ValidatePanelContourSpec(const PanelContourSpec &spec)
{
    if (!std::isfinite(spec.radius) || spec.radius < 0 || spec.radius > 256 ||
        !std::isfinite(spec.neck_width) || spec.neck_width < 0 || spec.neck_width > 256 ||
        !std::isfinite(spec.neck_height) || spec.neck_height < 0 || spec.neck_height > 48) {
        throw std::invalid_argument("Panel contour spec exceeds finite radius/neck limits");
    }

    switch (spec.neck_shape) {
    case PanelNeckShape::SoftTab:
    case PanelNeckShape::RoundedTriangle:
        break;
    default:
        throw std::invalid_argument("Unknown panel neck shape");
    }
}

double PanelNeckHeight(const PanelContourSpec &spec)
{
    ValidatePanelContourSpec(spec);
    return NeckHeight(spec);
}

std::optional<double> PanelAttachmentCenter(LogicalSize body, const PanelContourSpec &spec,
                                            double preferred, double anchor_left,
                                            double anchor_right)
{
    ValidateBody(body);
    ValidatePanelContourSpec(spec);
    if (!std::isfinite(preferred) || !std::isfinite(anchor_left) || !std::isfinite(anchor_right) ||
        anchor_left > anchor_right) {
        throw std::invalid_argument("Panel attachment requires a finite ordered anchor interval");
    }
    if (NeckHeight(spec) == 0) {
        return std::nullopt;
    }

    const auto [minimum, maximum] = AttachmentLimits(body, spec);
    const double left = std::max(minimum, anchor_left);
    const double right = std::min(maximum, anchor_right);
    if (left > right) {
        return std::nullopt;
    }
    return std::clamp(preferred, left, right);
}

Contour PreparePanelContour(const PanelContourRequest &request)
{
    const double height = ValidateRequest(request);
    auto path = BodyPath(request, height);
    if (request.edge == PanelAttachmentEdge::Bottom && height > 0) {
        MirrorPath(path, request.body.height);
    }

    auto contour = PrepareContour(path);
    if (height > 0) {
        const auto bounds = ContourBounds(contour);
        const double body_bottom = std::round(request.body.height * 256) / 256;
        const bool attached = request.edge == PanelAttachmentEdge::Top
                                  ? bounds.y < 0
                                  : bounds.y + bounds.height > body_bottom;
        if (!attached) {
            throw std::invalid_argument("Panel neck collapses during contour quantization");
        }
        if (request.spec.neck_shape == PanelNeckShape::RoundedTriangle) {
            const double tip_y =
                request.edge == PanelAttachmentEdge::Top ? bounds.y : bounds.y + bounds.height;
            const auto apex_count =
                std::count_if(contour.points.begin(), contour.points.end(),
                              [tip_y](LogicalPoint point) { return point.y == tip_y; });
            if (apex_count != 1) {
                throw std::invalid_argument("Panel triangular apex collapses during quantization");
            }
        }
    }
    return contour;
}

} // namespace prism::contracts
