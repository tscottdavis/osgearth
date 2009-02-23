/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
 * Copyright 2008-2009 Pelican Ventures, Inc.
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */

#include <osgEarth/MapConfig>
#include <osgEarth/Mercator>
#include <osgEarth/TileSource>
#include <osgEarth/ImageToHeightFieldConverter>
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/Registry>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <sstream>
#include <stdlib.h>
#include <iomanip>

#include "Capabilities"
#include "TileService"

using namespace osgEarth;

#define PROPERTY_URL              "url"
#define PROPERTY_CAPABILITIES_URL "capabilities_url"
#define PROPERTY_TILESERVICE_URL  "tileservice_url"
#define PROPERTY_LAYERS           "layers"
#define PROPERTY_STYLE            "style"
#define PROPERTY_FORMAT           "format"
#define PROPERTY_TILE_SIZE        "tile_size"
#define PROPERTY_ELEVATION_UNIT   "elevation_unit"
#define PROPERTY_SRS              "srs"
#define PROPERTY_MAP_CONFIG       "map_config"

class WMSSource : public TileSource
{
public:
	WMSSource( const osgDB::ReaderWriter::Options* options ):
	  tile_size(256),
      map_config(0)
    {
        if ( options->getPluginData( PROPERTY_URL ) )
            prefix = std::string( (const char*)options->getPluginData( PROPERTY_URL ) );

        if ( options->getPluginData( PROPERTY_LAYERS ) )
            layers = std::string( (const char*)options->getPluginData( PROPERTY_LAYERS ) );

        if ( options->getPluginData( PROPERTY_STYLE ) )
            style = std::string( (const char*)options->getPluginData( PROPERTY_STYLE ) );

        if ( options->getPluginData( PROPERTY_FORMAT ) )
            format = std::string( (const char*)options->getPluginData( PROPERTY_FORMAT ) );

        if ( options->getPluginData( PROPERTY_CAPABILITIES_URL ) )
             capabilitiesURL = std::string( (const char*)options->getPluginData( PROPERTY_CAPABILITIES_URL ) );

         if ( options->getPluginData( PROPERTY_TILESERVICE_URL ) )
             tileServiceURL = std::string( (const char*)options->getPluginData( PROPERTY_TILESERVICE_URL ) );

        if ( options->getPluginData( PROPERTY_MAP_CONFIG))
             map_config = (const MapConfig*)options->getPluginData( PROPERTY_MAP_CONFIG );

        if ( options->getPluginData( PROPERTY_ELEVATION_UNIT))
             elevation_unit = std::string( (const char*)options->getPluginData( PROPERTY_ELEVATION_UNIT ) );

		if ( options->getPluginData( PROPERTY_TILE_SIZE ) )
            tile_size = as<int>( (const char*)options->getPluginData( PROPERTY_TILE_SIZE ), 256 );

        if ( options->getPluginData( PROPERTY_SRS ) )
            srs = std::string( (const char*)options->getPluginData( PROPERTY_SRS ) );

        if ( elevation_unit.empty())
        {
            elevation_unit = "m";
        }


        char sep = prefix.find_first_of('?') == std::string::npos? '?' : '&';

        if (capabilitiesURL.empty()) capabilitiesURL = prefix + sep + "SERVICE=WMS&VERSION=1.1.1&REQUEST=GetCapabilities";

        //Try to read the WMS capabilities
        capabilities = CapabilitiesReader::read(capabilitiesURL);


        if (capabilities.valid())
        {
            osg::notify(osg::INFO) << "Got capabilities from " << capabilitiesURL << std::endl;
            if (format.empty())
            {
                format = capabilities->suggestExtension();
                osg::notify(osg::NOTICE) << "No format specified, capabilities suggested extension " << format << std::endl;
            }
        }
        else
        {
            osg::notify(osg::NOTICE) << "Could not read capabilities from WMS service using request " << capabilitiesURL << std::endl;
        }

        if ( format.empty() )
            format = "png";

       
        if ( srs.empty() )
        {
            srs = "EPSG:4326";
        }

        //Initialize the WMS request prototype
        std::stringstream buf;
        buf
            << std::fixed << prefix << sep
            << "SERVICE=WMS&VERSION=1.1.1&REQUEST=GetMap"
            << "&LAYERS=" << layers
            << "&FORMAT=image/" << format
            << "&STYLES=" << style
            << "&SRS=" << srs
            << "&WIDTH="<< tile_size
            << "&HEIGHT="<< tile_size
            << "&BBOX=%lf,%lf,%lf,%lf";
        prototype = buf.str();

        TileGridProfile::ProfileType profileType = TileGridProfile::getProfileTypeFromSRS(srs);

        //If the map profile already defined and the profile types are the same,
        //assume we will be compatible with the map profile.
        if (map_config->getProfile().getProfileType() == profileType)
        {
            profile = map_config->getProfile();
        }


        if (profile.getProfileType() == TileGridProfile::UNKNOWN)
        {
            if (profileType == TileGridProfile::PROJECTED)
            {
                if (capabilities.valid())
                {
                    Layer* layer = capabilities->getLayerByName(layers);
                    if (layer)
                    {
                        double minx, miny, maxx, maxy;
                        layer->getExtents(minx, miny, maxx, maxy);
                        profile = TileGridProfile(TileGridProfile::PROJECTED, minx, miny, maxx, maxy, srs);
                    }
                }
                else
                {
                    osg::notify(osg::NOTICE) << "Could not read WMS capabilities, no way of determining valid extents " << std::endl;
                }
            }
            else
            {
                //If the profile type is not projected, osgEarth will provide the global extents
                profile = TileGridProfile(profileType);
            }
        }
        
        //Try to read the TileService
        if (tileServiceURL.empty()) tileServiceURL = prefix + sep + "request=GetTileService";
        //osg::notify(osg::NOTICE) << "TileService URL " << tileServiceRequest << std::endl;
        tileService = TileServiceReader::read(tileServiceURL);
        if (tileService.valid())
        {
            osg::notify(osg::NOTICE) << "Read TileService " << std::endl;
            TileService::TilePatternList patterns;
            tileService->getMatchingPatterns(layers, format, style, srs, tile_size, tile_size, patterns);

            if (patterns.size() > 0)
            {
                profile = tileService->getProfile(patterns);
                prototype = prefix + sep + patterns[0].getPrototype();
            }
        }
        else
        {
            osg::notify(osg::INFO) << "Could not read TileService " << std::endl;
        }

        prototype = prototype + "&." + format;


    }

    const TileGridProfile& getProfile() const
    {
        return profile;
    }

    osg::Image* createImage( const TileKey* key )
    {
        std::string uri = createURI( key );
        return osgDB::readImageFile( uri );
    }

    osg::HeightField* createHeightField( const TileKey* key )
    {
        osg::Image* image = createImage(key);
        if (!image)
        {
            osg::notify(osg::INFO) << "Failed to read heightfield from " << createURI(key) << std::endl;
        }

        float scaleFactor = 1;

        //Scale the heightfield to meters
        if (elevation_unit == "ft")
        {
            scaleFactor = 0.3048;
        }
        return (ImageToHeightFieldConverter::convert(image, scaleFactor));        
    }

    std::string createURI( const TileKey* key ) const
    {
        double minx, miny, maxx, maxy;
        key->getGeoExtents( minx, miny, maxx, maxy);
        // http://labs.metacarta.com/wms-c/Basic.py?SERVICE=WMS&VERSION=1.1.1&REQUEST=GetMap&LAYERS=basic&BBOX=-180,-90,0,90
        char buf[2048];
        sprintf(buf, prototype.c_str(), minx, miny, maxx, maxy);
        return buf;
    }

    virtual int getPixelsPerTile() const
    {
        return tile_size;
    }

    virtual std::string getExtension()  const 
    {
        return format;
    }

private:
    std::string prefix;
    std::string layers;
    std::string style;
    std::string format;
    std::string srs;
    std::string tileServiceURL;
    std::string capabilitiesURL;
	int tile_size;
    const MapConfig* map_config;
    std::string elevation_unit;
    osg::ref_ptr<Capabilities> capabilities;
    osg::ref_ptr<TileService> tileService;
    TileGridProfile profile;
    std::string prototype;
};


class ReaderWriterWMS : public osgDB::ReaderWriter
{
    public:
        ReaderWriterWMS() {}

        virtual const char* className()
        {
            return "WMS Reader";
        }
        
        virtual bool acceptsExtension(const std::string& extension) const
        {
            return osgDB::equalCaseInsensitive( extension, "osgearth_wms" );
        }

        virtual ReadResult readObject(const std::string& file_name, const Options* opt) const
        {
            std::string ext = osgDB::getFileExtension( file_name );
            if ( !acceptsExtension( ext ) )
            {
                return ReadResult::FILE_NOT_HANDLED;
            }

            return new WMSSource(opt);
        }
};

REGISTER_OSGPLUGIN(osgearth_wms, ReaderWriterWMS)
