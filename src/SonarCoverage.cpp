#include "survey_manager/SonarCoverage.h"
#include <pluginlib/class_list_macros.h>
#include "project11/gz4d_geo.h"
#include "geographic_msgs/GeoPath.h"
#include <geos/geom/CoordinateArraySequence.h>
#include <geos/operation/union/CascadedPolygonUnion.h>
#include <geos/simplify/DouglasPeuckerSimplifier.h>
#include <geos/geom/Polygon.h>
#include <geos/geom/LineString.h>

PLUGINLIB_EXPORT_CLASS(survey_manager::SonarCoverage, nodelet::Nodelet)

namespace survey_manager
{
    SonarCoverage::SonarCoverage():m_interval(5.0),m_alongship_beamwidth(5.0),m_port_angle(75.0),m_starboard_angle(75.0),m_interval_accumulated_distance(0.0)
    {
        m_half_alongship_beamwidth_tan = tan(m_alongship_beamwidth*M_PI/360.0);
        m_port_tan = tan(m_port_angle*M_PI/180.0);
        m_starboard_tan = tan(m_starboard_angle*M_PI/180.0);
    }

    
    void SonarCoverage::onInit()
    {
        NODELET_DEBUG("Initializing nodelet...");
        
        m_geometry_factory = geos::geom::GeometryFactory::getDefaultInstance();
        
        auto node = getNodeHandle();
        m_depth_sub = node.subscribe("/depth",10,&SonarCoverage::depthCallback, this);
        m_heading_sub = node.subscribe("/heading",10,&SonarCoverage::headingCallback, this);
        m_position_sub = node.subscribe("/position",10,&SonarCoverage::positionCallback, this);
        m_reset_sub = node.subscribe("/sim_reset",10,&SonarCoverage::resetCallback, this);
        
        m_coverage_pub = node.advertise<geographic_msgs::GeoPath>("/coverage",10);
        m_mbes_ping_pub = node.advertise<geographic_msgs::GeoPath>("/mbes_ping",10);
    }
    
    void SonarCoverage::depthCallback(std_msgs::Float32::ConstPtr data)
    {
        ros::Time now = ros::Time::now();
        if(now-m_last_heading_time < ros::Duration(.5) && now-m_last_position_time < ros::Duration(.5))
        {
            std::cerr << "SonarCoverage: depth: " << data->data << ", position: " << m_latitude << ", " << m_longitude << ", heading: " << m_heading << std::endl;
            PingRecord pr;
            pr.heading = m_heading;
            pr.nadir_latitude = m_latitude;
            pr.nadir_longitude = m_longitude;
            pr.port_distance = data->data*m_port_tan;
            pr.starboard_distance = data->data*m_starboard_tan;
            if(!m_interval_record.empty())
            {
                gz4d::geo::Point<double,gz4d::geo::WGS84::LatLon> p1, p2;
                p1[0] = m_interval_record.back().nadir_latitude;
                p1[1] = m_interval_record.back().nadir_longitude;
                p2[0] = pr.nadir_latitude;
                p2[1] = pr.nadir_longitude;
                
                auto azimuth_distance = gz4d::geo::WGS84::Ellipsoid::inverse(p1,p2);
                m_interval_accumulated_distance += azimuth_distance.second;
                std::cerr << "distance: " << m_interval_accumulated_distance << std::endl;
            }
            m_interval_record.push_back(pr);
            if(m_interval_accumulated_distance > m_interval)
                processInterval();
            
            // send ping as rectangle representing it's footprint.
            geos::geom::CoordinateSequence * coordinates = new geos::geom::CoordinateArraySequence();
            gz4d::geo::Point<double,gz4d::geo::WGS84::LatLon> nadir;
            nadir[0] = pr.nadir_latitude;
            nadir[1] = pr.nadir_longitude;
            auto starboard_point = gz4d::geo::WGS84::Ellipsoid::direct(nadir,pr.heading+90,pr.starboard_distance);
            double alongship_half_distance = data->data * m_half_alongship_beamwidth_tan;
            std::cerr << "alongship_half_distance: " << alongship_half_distance << std::endl;
            auto starboard_fwd_point = gz4d::geo::WGS84::Ellipsoid::direct(starboard_point,pr.heading,alongship_half_distance);
            geos::geom::Coordinate coordinate;
            coordinate.x = starboard_fwd_point[1];
            coordinate.y = starboard_fwd_point[0];
            std::cerr << "stbd fwd: " << coordinate.toString() << std::endl;
            coordinates->add(coordinate);
            auto port_fwd_point = gz4d::geo::WGS84::Ellipsoid::direct(starboard_fwd_point,pr.heading-90,pr.starboard_distance+pr.port_distance);
            coordinate.x = port_fwd_point[1];
            coordinate.y = port_fwd_point[0];
            std::cerr << "port fwd: " << coordinate.toString() << std::endl;
            coordinates->add(coordinate);
            auto port_back_point = gz4d::geo::WGS84::Ellipsoid::direct(port_fwd_point,pr.heading+180,2*alongship_half_distance);
            coordinate.x = port_back_point[1];
            coordinate.y = port_back_point[0];
            std::cerr << "port back: " << coordinate.toString() << std::endl;
            coordinates->add(coordinate);
            auto starboard_back_point = gz4d::geo::WGS84::Ellipsoid::direct(port_back_point,pr.heading+90,pr.starboard_distance+pr.port_distance);
            coordinate.x = starboard_back_point[1];
            coordinate.y = starboard_back_point[0];
            std::cerr << "stbd back: " << coordinate.toString() << std::endl;
            coordinates->add(coordinate);
            coordinates->add(coordinates->front());
            auto new_poly = m_geometry_factory->createPolygon(m_geometry_factory->createLinearRing(coordinates),nullptr);
            m_coverage.push_back(new_poly); 
            auto union_op = new geos::operation::geounion::CascadedPolygonUnion(&m_coverage);
            auto new_coverage = union_op->Union();
            std::cerr << "new coverage type: " << new_coverage->getGeometryType() << std::endl;

            geos::simplify::DouglasPeuckerSimplifier simplifier(new_coverage);
            simplifier.setDistanceTolerance(0.0001);
            auto new_simplified_coverage = simplifier.getResultGeometry();

            std::cerr << "new simplified coverage type: " << new_simplified_coverage->getGeometryType() << std::endl;

            //std::cerr << new_coverage->toString() << std::endl;
            geos::geom::Polygon *new_polygon = dynamic_cast<geos::geom::Polygon*>(new_simplified_coverage.get());
            if (new_polygon)
            {
                m_coverage.clear();
                m_coverage.push_back(new_polygon);
                publishCoverage();
            }
            geos::geom::MultiPolygon *new_multiPolygon = dynamic_cast<geos::geom::MultiPolygon*>(new_simplified_coverage.get());
            if (new_multiPolygon)
            {
                m_coverage.clear();
                for(auto p: *new_multiPolygon)
                {
                    geos::geom::Polygon *polygon = dynamic_cast<geos::geom::Polygon*>(p);
                    m_coverage.push_back(polygon);
                }
                publishCoverage();
            }            
        }
        else
            std::cerr << "SonarCoverage: depth: " << data->data << std::endl;
    }
    
    void SonarCoverage::headingCallback(marine_msgs::NavEulerStamped::ConstPtr data)
    {
        m_heading = data->orientation.heading;
        m_last_heading_time = data->header.stamp;
        //std::cerr << "SonarCoverage: heading: " << data->orientation.heading << std::endl;
    }

    void SonarCoverage::positionCallback(geographic_msgs::GeoPointStamped::ConstPtr data)
    {
        m_latitude = data->position.latitude;
        m_longitude = data->position.longitude;
        m_last_position_time = data->header.stamp;
        //std::cerr << "SonarCoverage: position: " << data->position.latitude << ", " << data->position.longitude << std::endl;
    }
    
    void SonarCoverage::processInterval()
    {
        if(!m_interval_record.empty())
        {
            double min_distance_port = m_interval_record.front().port_distance;
            double min_distance_starboard = m_interval_record.front().starboard_distance;
            for(auto ir: m_interval_record)
            {
                min_distance_port = std::min(min_distance_port,ir.port_distance);
                min_distance_starboard = std::min(min_distance_starboard,ir.starboard_distance);
            }
            if(m_pings.empty())
            {
                PingRecord pr = m_interval_record.front();
                pr.port_distance = min_distance_port;
                pr.starboard_distance = min_distance_starboard;
                m_pings.push_back(pr);
            }
            PingRecord pr = m_interval_record.back();
            pr.port_distance = min_distance_port;
            pr.starboard_distance = min_distance_starboard;
            m_pings.push_back(pr);
            m_interval_record.clear();
            m_interval_accumulated_distance = 0.0;
            publishCoverage();
        }
    }

    void SonarCoverage::publishCoverage()
    {
        geographic_msgs::GeoPath gpath;
        for(auto p: m_coverage)
        {
            auto er = p->getExteriorRing();
            auto cs = er->getCoordinates();
            auto csv = cs->toVector();
            for(auto p: *csv)
            {
                geographic_msgs::GeoPoseStamped gpose;
                gpose.pose.position.latitude = p.y;
                gpose.pose.position.longitude = p.x;
                gpath.poses.push_back(gpose); 
            }
            geographic_msgs::GeoPoseStamped gpose;
            gpose.pose.position.latitude = -91;
            gpose.pose.position.longitude = -181;
            gpath.poses.push_back(gpose); 
        }
        m_coverage_pub.publish(gpath);
        return;

        if(!m_pings.empty())
        {
            geographic_msgs::GeoPath gpath;
            std::vector<gz4d::geo::Point<double,gz4d::geo::WGS84::LatLon> > port_positions;
            for(PingRecord p: m_pings)
            {
                gz4d::geo::Point<double,gz4d::geo::WGS84::LatLon> nadir;
                nadir[0] = p.nadir_latitude;
                nadir[1] = p.nadir_longitude;
                auto starboard_point = gz4d::geo::WGS84::Ellipsoid::direct(nadir,p.heading+90,p.starboard_distance);
                geographic_msgs::GeoPoseStamped gpose;
                gpose.pose.position.latitude = starboard_point[0];
                gpose.pose.position.longitude = starboard_point[1];
                gpath.poses.push_back(gpose);
                port_positions.push_back(gz4d::geo::WGS84::Ellipsoid::direct(nadir,p.heading-90,p.port_distance));
            }
            for(auto p = port_positions.rbegin(); p!=port_positions.rend();p++)
            {
                geographic_msgs::GeoPoseStamped gpose;
                gpose.pose.position.latitude = (*p)[0];
                gpose.pose.position.longitude = (*p)[1];
                gpath.poses.push_back(gpose);
            }
            m_coverage_pub.publish(gpath);
        }
    }
    
    void SonarCoverage::resetCallback(std_msgs::Bool::ConstPtr data)
    {
        m_interval_record.clear();
        m_interval_accumulated_distance = 0.0;
        m_pings.clear();
        publishCoverage();
    }


}
