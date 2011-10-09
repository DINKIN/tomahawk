/* === This file is part of Tomahawk Player - <http://tomahawk-player.org> ===
 *
 *   Copyright 2010-2011, Christian Muehlhaeuser <muesli@tomahawk-player.org>
 *
 *   Tomahawk is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   Tomahawk is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Tomahawk. If not, see <http://www.gnu.org/licenses/>.
 */

#include "lastfmplugin.h"

#include <QDir>
#include <QSettings>
#include <QCryptographicHash>
#include <QNetworkConfiguration>
#include <QDomElement>

#include "album.h"
#include "typedefs.h"
#include "audio/audioengine.h"
#include "tomahawksettings.h"
#include "utils/tomahawkutils.h"
#include "utils/logger.h"

#include <lastfm/ws.h>
#include <lastfm/XmlQuery>

#include <qjson/parser.h>

using namespace Tomahawk::InfoSystem;

static QString
md5( const QByteArray& src )
{
    QByteArray const digest = QCryptographicHash::hash( src, QCryptographicHash::Md5 );
    return QString::fromLatin1( digest.toHex() ).rightJustified( 32, '0' );
}


LastFmPlugin::LastFmPlugin()
    : InfoPlugin()
    , m_scrobbler( 0 )
{
    m_supportedGetTypes << InfoAlbumCoverArt << InfoArtistImages << InfoArtistSimilars << InfoArtistSongs << InfoChart << InfoChartCapabilities;
    m_supportedPushTypes << InfoSubmitScrobble << InfoSubmitNowPlaying << InfoLove << InfoUnLove;

/*
      Your API Key is 7194b85b6d1f424fe1668173a78c0c4a
      Your secret is ba80f1df6d27ae63e9cb1d33ccf2052f
*/

    // Flush session key cache
    TomahawkSettings::instance()->setLastFmSessionKey( QByteArray() );

    lastfm::ws::ApiKey = "7194b85b6d1f424fe1668173a78c0c4a";
    lastfm::ws::SharedSecret = "ba80f1df6d27ae63e9cb1d33ccf2052f";
    lastfm::ws::Username = TomahawkSettings::instance()->lastFmUsername();

    m_pw = TomahawkSettings::instance()->lastFmPassword();

    //HACK work around a bug in liblastfm---it doesn't create its config dir, so when it
    // tries to write the track cache, it fails silently. until we have a fixed version, do this
    // code taken from Amarok (src/services/lastfm/ScrobblerAdapter.cpp)
#ifdef Q_WS_X11
    QString lpath = QDir::home().filePath( ".local/share/Last.fm" );
    QDir ldir = QDir( lpath );
    if( !ldir.exists() )
    {
        ldir.mkpath( lpath );
    }
#endif

    m_badUrls << QUrl( "http://cdn.last.fm/flatness/catalogue/noimage" );

    connect( TomahawkSettings::instance(), SIGNAL( changed() ),
                                             SLOT( settingsChanged() ), Qt::QueuedConnection );
}


LastFmPlugin::~LastFmPlugin()
{
    qDebug() << Q_FUNC_INFO;
    delete m_scrobbler;
    m_scrobbler = 0;
}


void
LastFmPlugin::namChangedSlot( QNetworkAccessManager *nam )
{
    if ( !nam )
        return;

    TomahawkUtils::NetworkProxyFactory* oldProxyFactory = dynamic_cast< TomahawkUtils::NetworkProxyFactory* >( nam->proxyFactory() );
    if ( !oldProxyFactory )
    {
        tLog() << Q_FUNC_INFO << "Could not get old proxyFactory!";
        return;
    }

    //WARNING: there's a chance liblastfm2 will clobber the application proxy factory it if it constructs a nam due to the below call
    //but it is unsafe to re-set it here
    QNetworkAccessManager* currNam = lastfm::nam();

    currNam->setConfiguration( nam->configuration() );
    currNam->setNetworkAccessible( nam->networkAccessible() );
    TomahawkUtils::NetworkProxyFactory* newProxyFactory = new TomahawkUtils::NetworkProxyFactory();
    newProxyFactory->setNoProxyHosts( oldProxyFactory->noProxyHosts() );
    QNetworkProxy newProxy( oldProxyFactory->proxy() );
    newProxyFactory->setProxy( newProxy );
    currNam->setProxyFactory( newProxyFactory );
    settingsChanged(); // to get the scrobbler set up
}


void
LastFmPlugin::dataError( uint requestId, Tomahawk::InfoSystem::InfoRequestData requestData )
{
    emit info( requestId, requestData, QVariant() );
    return;
}


void
LastFmPlugin::getInfo( uint requestId, Tomahawk::InfoSystem::InfoRequestData requestData )
{
    switch ( requestData.type )
    {
        case InfoArtistImages:
            fetchArtistImages( requestId, requestData );
            break;

        case InfoAlbumCoverArt:
            fetchCoverArt( requestId, requestData );
            break;

        case InfoArtistSimilars:
            fetchSimilarArtists( requestId, requestData );
            break;

        case InfoArtistSongs:
            fetchTopTracks( requestId, requestData );
            break;

        case InfoChart:
            fetchChart( requestId, requestData );
            break;

        case InfoChartCapabilities:
            fetchChartCapabilities( requestId, requestData );
            break;
        default:
            dataError( requestId, requestData );
    }
}


void
LastFmPlugin::pushInfo( const QString caller, const Tomahawk::InfoSystem::InfoType type, const QVariant input )
{
    Q_UNUSED( caller )
    switch ( type )
    {
        case InfoSubmitNowPlaying:
            nowPlaying( input );
            break;

        case InfoSubmitScrobble:
            scrobble();
            break;

        case InfoLove:
        case InfoUnLove:
            sendLoveSong( type, input );
            break;

        default:
            return;
    }
}


void
LastFmPlugin::nowPlaying( const QVariant &input )
{
    if ( !input.canConvert< Tomahawk::InfoSystem::InfoCriteriaHash >() || !m_scrobbler )
    {
        tLog() << "LastFmPlugin::nowPlaying no m_scrobbler, or cannot convert input!";
        if ( !m_scrobbler )
            tLog() << "No scrobbler!";
        return;
    }

    InfoCriteriaHash hash = input.value< Tomahawk::InfoSystem::InfoCriteriaHash >();
    if ( !hash.contains( "title" ) || !hash.contains( "artist" ) || !hash.contains( "album" ) || !hash.contains( "duration" ) )
        return;

    m_track = lastfm::MutableTrack();
    m_track.stamp();

    m_track.setTitle( hash["title"] );
    m_track.setArtist( hash["artist"] );
    m_track.setAlbum( hash["album"] );
    bool ok;
    m_track.setDuration( hash["duration"].toUInt( &ok ) );
    m_track.setSource( lastfm::Track::Player );

    m_scrobbler->nowPlaying( m_track );
}


void
LastFmPlugin::scrobble()
{
    if ( !m_scrobbler || m_track.isNull() )
        return;

    tLog() << Q_FUNC_INFO << "Scrobbling now:" << m_track.toString();
    m_scrobbler->cache( m_track );
    m_scrobbler->submit();
}


void
LastFmPlugin::sendLoveSong( const InfoType type, QVariant input )
{
    qDebug() << Q_FUNC_INFO;

    if ( !input.canConvert< Tomahawk::InfoSystem::InfoCriteriaHash >() )
    {
        tLog() << "LastFmPlugin::nowPlaying cannot convert input!";
        return;
    }

    InfoCriteriaHash hash = input.value< Tomahawk::InfoSystem::InfoCriteriaHash >();
    if ( !hash.contains( "title" ) || !hash.contains( "artist" ) || !hash.contains( "album" ) )
        return;

    lastfm::MutableTrack track;
    track.stamp();

    track.setTitle( hash["title"] );
    track.setArtist( hash["artist"] );
    track.setAlbum( hash["album"] );
    bool ok;
    track.setDuration( hash["duration"].toUInt( &ok ) );
    track.setSource( lastfm::Track::Player );

    if ( type == Tomahawk::InfoSystem::InfoLove )
    {
        track.love();
    }
    else if ( type == Tomahawk::InfoSystem::InfoUnLove )
    {
        track.unlove();
    }
}


void
LastFmPlugin::fetchSimilarArtists( uint requestId, Tomahawk::InfoSystem::InfoRequestData requestData )
{
    if ( !requestData.input.canConvert< Tomahawk::InfoSystem::InfoCriteriaHash >() )
    {
        dataError( requestId, requestData );
        return;
    }
    InfoCriteriaHash hash = requestData.input.value< Tomahawk::InfoSystem::InfoCriteriaHash >();
    if ( !hash.contains( "artist" ) )
    {
        dataError( requestId, requestData );
        return;
    }

    Tomahawk::InfoSystem::InfoCriteriaHash criteria;
    criteria["artist"] = hash["artist"];

    emit getCachedInfo( requestId, criteria, 2419200000, requestData );
}


void
LastFmPlugin::fetchTopTracks( uint requestId, Tomahawk::InfoSystem::InfoRequestData requestData )
{
    if ( !requestData.input.canConvert< Tomahawk::InfoSystem::InfoCriteriaHash >() )
    {
        dataError( requestId, requestData );
        return;
    }
    InfoCriteriaHash hash = requestData.input.value< Tomahawk::InfoSystem::InfoCriteriaHash >();
    if ( !hash.contains( "artist" ) )
    {
        dataError( requestId, requestData );
        return;
    }

    Tomahawk::InfoSystem::InfoCriteriaHash criteria;
    criteria["artist"] = hash["artist"];

    emit getCachedInfo( requestId, criteria, 2419200000, requestData );
}

void
LastFmPlugin::fetchChart( uint requestId, Tomahawk::InfoSystem::InfoRequestData requestData )
{
    if ( !requestData.input.canConvert< Tomahawk::InfoSystem::InfoCriteriaHash >() )
    {
        dataError( requestId, requestData );
        return;
    }
    InfoCriteriaHash hash = requestData.input.value< Tomahawk::InfoSystem::InfoCriteriaHash >();
    Tomahawk::InfoSystem::InfoCriteriaHash criteria;
    if ( !hash.contains( "chart_id" ) )
    {
        dataError( requestId, requestData );
        return;
    } else {
        criteria["chart_id"] = hash["chart_id"];
    }

    emit getCachedInfo( requestId, criteria, 0, requestData );
}

void
LastFmPlugin::fetchChartCapabilities( uint requestId, Tomahawk::InfoSystem::InfoRequestData requestData )
{
    if ( !requestData.input.canConvert< Tomahawk::InfoSystem::InfoCriteriaHash >() )
    {
        dataError( requestId, requestData );
        return;
    }
    InfoCriteriaHash hash = requestData.input.value< Tomahawk::InfoSystem::InfoCriteriaHash >();
    Tomahawk::InfoSystem::InfoCriteriaHash criteria;

    emit getCachedInfo( requestId, criteria, 0, requestData );
}

void
LastFmPlugin::fetchCoverArt( uint requestId, Tomahawk::InfoSystem::InfoRequestData requestData )
{
    if ( !requestData.input.canConvert< Tomahawk::InfoSystem::InfoCriteriaHash >() )
    {
        dataError( requestId, requestData );
        return;
    }
    InfoCriteriaHash hash = requestData.input.value< Tomahawk::InfoSystem::InfoCriteriaHash >();
    if ( !hash.contains( "artist" ) || !hash.contains( "album" ) )
    {
        dataError( requestId, requestData );
        return;
    }

    Tomahawk::InfoSystem::InfoCriteriaHash criteria;
    criteria["artist"] = hash["artist"];
    criteria["album"] = hash["album"];

    emit getCachedInfo( requestId, criteria, 2419200000, requestData );
}


void
LastFmPlugin::fetchArtistImages( uint requestId, Tomahawk::InfoSystem::InfoRequestData requestData )
{
    if ( !requestData.input.canConvert< Tomahawk::InfoSystem::InfoCriteriaHash >() )
    {
        dataError( requestId, requestData );
        return;
    }
    InfoCriteriaHash hash = requestData.input.value< Tomahawk::InfoSystem::InfoCriteriaHash >();
    if ( !hash.contains( "artist" ) )
    {
        dataError( requestId, requestData );
        return;
    }

    Tomahawk::InfoSystem::InfoCriteriaHash criteria;
    criteria["artist"] = hash["artist"];

    emit getCachedInfo( requestId, criteria, 2419200000, requestData );
}


void
LastFmPlugin::notInCacheSlot( uint requestId, QHash<QString, QString> criteria, Tomahawk::InfoSystem::InfoRequestData requestData )
{
    if ( !lastfm::nam() )
    {
        tLog() << "Have a null QNAM, uh oh";
        emit info( requestId, requestData, QVariant() );
        return;
    }

    switch ( requestData.type )
    {
        case InfoChart:
        {
            tDebug() << "LastFmPlugin: InfoChart not in cache, fetching";
            QMap<QString, QString> args;
            tDebug() << "LastFmPlugin: " << "args chart_id" << criteria["chart_id"];
            args["method"] = criteria["chart_id"];
            args["limit"] = "100";
            QNetworkReply* reply = lastfm::ws::get(args);
            reply->setProperty( "requestId", requestId );
            reply->setProperty( "requestData", QVariant::fromValue< Tomahawk::InfoSystem::InfoRequestData >( requestData ) );

            connect( reply, SIGNAL( finished() ), SLOT( chartReturned() ) );
            return;
        }

        case InfoArtistSimilars:
        {
            lastfm::Artist a( criteria["artist"] );
            QNetworkReply* reply = a.getSimilar();
            reply->setProperty( "requestId", requestId );
            reply->setProperty( "requestData", QVariant::fromValue< Tomahawk::InfoSystem::InfoRequestData >( requestData ) );

            connect( reply, SIGNAL( finished() ), SLOT( similarArtistsReturned() ) );
            return;
        }

        case InfoArtistSongs:
        {
            lastfm::Artist a( criteria["artist"] );
            QNetworkReply* reply = a.getTopTracks();
            reply->setProperty( "requestId", requestId );
            reply->setProperty( "requestData", QVariant::fromValue< Tomahawk::InfoSystem::InfoRequestData >( requestData ) );

            connect( reply, SIGNAL( finished() ), SLOT( topTracksReturned() ) );
            return;
        }

        case InfoAlbumCoverArt:
        {
            QString artistName = criteria["artist"];
            QString albumName = criteria["album"];

            QString imgurl = "http://ws.audioscrobbler.com/2.0/?method=album.imageredirect&artist=%1&album=%2&autocorrect=1&size=large&api_key=7a90f6672a04b809ee309af169f34b8b";
            QNetworkRequest req( imgurl.arg( artistName ).arg( albumName ) );
            QNetworkReply* reply = lastfm::nam()->get( req );
            reply->setProperty( "requestId", requestId );
            reply->setProperty( "requestData", QVariant::fromValue< Tomahawk::InfoSystem::InfoRequestData >( requestData ) );

            connect( reply, SIGNAL( finished() ), SLOT( coverArtReturned() ) );
            return;
        }

        case InfoArtistImages:
        {
            QString artistName = criteria["artist"];

            QString imgurl = "http://ws.audioscrobbler.com/2.0/?method=artist.imageredirect&artist=%1&autocorrect=1&size=large&api_key=7a90f6672a04b809ee309af169f34b8b";
            QNetworkRequest req( imgurl.arg( artistName ) );
            QNetworkReply* reply = lastfm::nam()->get( req );
            reply->setProperty( "requestId", requestId );
            reply->setProperty( "requestData", QVariant::fromValue< Tomahawk::InfoSystem::InfoRequestData >( requestData ) );

            connect( reply, SIGNAL( finished() ), SLOT( artistImagesReturned() ) );
            return;
        }

        default:
        {
            tLog() << Q_FUNC_INFO << "Couldn't figure out what to do with this type of request after cache miss";
            emit info( requestId, requestData, QVariant() );
            return;
        }
    }
}


void
LastFmPlugin::similarArtistsReturned()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>( sender() );

    QMap< int, QString > similarArtists = lastfm::Artist::getSimilar( reply );
    QStringList al;
    QStringList sl;

    foreach ( const QString& a, similarArtists.values() )
        al << a;

    QVariantMap returnedData;
    returnedData["artists"] = al;
    returnedData["score"] = sl;

    Tomahawk::InfoSystem::InfoRequestData requestData = reply->property( "requestData" ).value< Tomahawk::InfoSystem::InfoRequestData >();

    emit info(
        reply->property( "requestId" ).toUInt(),
        requestData,
        returnedData
    );

    Tomahawk::InfoSystem::InfoCriteriaHash origData = requestData.input.value< Tomahawk::InfoSystem::InfoCriteriaHash>();
    Tomahawk::InfoSystem::InfoCriteriaHash criteria;
    criteria["artist"] = origData["artist"];
    emit updateCache( criteria, 2419200000, requestData.type, returnedData );
}


void
LastFmPlugin::chartReturned()
{
    tDebug() << "LastfmPlugin: InfoChart data returned!";
    QNetworkReply* reply = qobject_cast<QNetworkReply*>( sender() );

    QVariantMap returnedData;
    const QRegExp tracks_rx( "chart\\.\\S+tracks\\S*", Qt::CaseInsensitive );
    const QRegExp artists_rx( "chart\\.\\S+artists\\S*", Qt::CaseInsensitive );
    const QString url = reply->url().toString();

    if ( url.contains( tracks_rx ) )
    {
        QList<lastfm::Track> tracks = parseTrackList( reply );
        QList<ArtistTrackPair> top_tracks;
        foreach( const lastfm::Track &t, tracks ) {
            ArtistTrackPair pair;
            pair.artist = t.artist().toString();
            pair.track = t.title();
            top_tracks << pair;
        }
        tDebug() << "LastFmPlugin:" << "\tgot " << top_tracks.size() << " tracks";
        returnedData["tracks"] = QVariant::fromValue( top_tracks );
        returnedData["type"] = "tracks";

    }
    else if ( url.contains( artists_rx ) )
    {
        QList<lastfm::Artist> list = lastfm::Artist::list( reply );
        QStringList al;
        tDebug() << "LastFmPlugin:"<< "\tgot " << list.size() << " artists";
        foreach ( const lastfm::Artist& a, list )
            al << a.toString();
        returnedData["artists"] = al;
        returnedData["type"] = "artists";
    }
    else
    {
        tDebug() << "LastfmPlugin:: got non tracks and non artists";
    }

    Tomahawk::InfoSystem::InfoRequestData requestData = reply->property( "requestData" ).value< Tomahawk::InfoSystem::InfoRequestData >();

    emit info(
        reply->property( "requestId" ).toUInt(),
        requestData,
        returnedData
    );
    // TODO update cache
}


void
LastFmPlugin::topTracksReturned()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>( sender() );

    QStringList topTracks = lastfm::Artist::getTopTracks( reply );
    QVariantMap returnedData;
    returnedData["tracks"] = topTracks;

    Tomahawk::InfoSystem::InfoRequestData requestData = reply->property( "requestData" ).value< Tomahawk::InfoSystem::InfoRequestData >();

    emit info(
        reply->property( "requestId" ).toUInt(),
        requestData,
        returnedData
    );

    Tomahawk::InfoSystem::InfoCriteriaHash origData = requestData.input.value< Tomahawk::InfoSystem::InfoCriteriaHash>();
    Tomahawk::InfoSystem::InfoCriteriaHash criteria;
    criteria["artist"] = origData["artist"];
    emit updateCache( criteria, 0, requestData.type, returnedData );
}


void
LastFmPlugin::coverArtReturned()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>( sender() );
    QUrl redir = reply->attribute( QNetworkRequest::RedirectionTargetAttribute ).toUrl();
    if ( redir.isEmpty() )
    {
        QByteArray ba = reply->readAll();
        if ( ba.isNull() || !ba.length() )
        {
            tLog() << Q_FUNC_INFO << "Uh oh, null byte array";
            emit info( reply->property( "requestId" ).toUInt(), reply->property( "requestData" ).value< Tomahawk::InfoSystem::InfoRequestData >(), QVariant() );
            return;
        }
        foreach ( const QUrl& url, m_badUrls )
        {
            if ( reply->url().toString().startsWith( url.toString() ) )
                ba = QByteArray();
        }

        QVariantMap returnedData;
        returnedData["imgbytes"] = ba;
        returnedData["url"] = reply->url().toString();

        Tomahawk::InfoSystem::InfoRequestData requestData = reply->property( "requestData" ).value< Tomahawk::InfoSystem::InfoRequestData >();

        emit info(
            reply->property( "requestId" ).toUInt(),
            requestData,
            returnedData
        );

        Tomahawk::InfoSystem::InfoCriteriaHash origData = requestData.input.value< Tomahawk::InfoSystem::InfoCriteriaHash>();
        Tomahawk::InfoSystem::InfoCriteriaHash criteria;
        criteria["artist"] = origData["artist"];
        criteria["album"] = origData["album"];
        emit updateCache( criteria, 2419200000, requestData.type, returnedData );
    }
    else
    {
        if ( !lastfm::nam() )
        {
            tLog() << Q_FUNC_INFO << "Uh oh, nam is null";
            emit info( reply->property( "requestId" ).toUInt(), reply->property( "requestData" ).value< Tomahawk::InfoSystem::InfoRequestData >(), QVariant() );
            return;
        }
        // Follow HTTP redirect
        QNetworkRequest req( redir );
        QNetworkReply* newReply = lastfm::nam()->get( req );
        newReply->setProperty( "requestId", reply->property( "requestId" ) );
        newReply->setProperty( "requestData", reply->property( "requestData" ) );
        connect( newReply, SIGNAL( finished() ), SLOT( coverArtReturned() ) );
    }

    reply->deleteLater();
}


void
LastFmPlugin::artistImagesReturned()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>( sender() );
    QUrl redir = reply->attribute( QNetworkRequest::RedirectionTargetAttribute ).toUrl();
    if ( redir.isEmpty() )
    {
        QByteArray ba = reply->readAll();
        if ( ba.isNull() || !ba.length() )
        {
            tLog() << Q_FUNC_INFO << "Uh oh, null byte array";
            emit info( reply->property( "requestId" ).toUInt(), reply->property( "requestData" ).value< Tomahawk::InfoSystem::InfoRequestData >(), QVariant() );
            return;
        }
        foreach ( const QUrl& url, m_badUrls )
        {
            if ( reply->url().toString().startsWith( url.toString() ) )
                ba = QByteArray();
        }
        QVariantMap returnedData;
        returnedData["imgbytes"] = ba;
        returnedData["url"] = reply->url().toString();

        Tomahawk::InfoSystem::InfoRequestData requestData = reply->property( "requestData" ).value< Tomahawk::InfoSystem::InfoRequestData >();

        emit info( reply->property( "requestId" ).toUInt(), requestData, returnedData );

        Tomahawk::InfoSystem::InfoCriteriaHash origData = requestData.input.value< Tomahawk::InfoSystem::InfoCriteriaHash>();
        Tomahawk::InfoSystem::InfoCriteriaHash criteria;
        criteria["artist"] = origData["artist"];
        emit updateCache( criteria, 2419200000, requestData.type, returnedData );
    }
    else
    {
        if ( !lastfm::nam() )
        {
            tLog() << Q_FUNC_INFO << "Uh oh, nam is null";
            emit info( reply->property( "requestId" ).toUInt(), reply->property( "requestData" ).value< Tomahawk::InfoSystem::InfoRequestData >(), QVariant() );
            return;
        }
        // Follow HTTP redirect
        QNetworkRequest req( redir );
        QNetworkReply* newReply = lastfm::nam()->get( req );
        newReply->setProperty( "requestId", reply->property( "requestId" ) );
        newReply->setProperty( "requestData", reply->property( "requestData" ) );
        connect( newReply, SIGNAL( finished() ), SLOT( artistImagesReturned() ) );
    }

    reply->deleteLater();
}


void
LastFmPlugin::settingsChanged()
{
    if( !m_scrobbler && TomahawkSettings::instance()->scrobblingEnabled() )
    { // can simply create the scrobbler
        lastfm::ws::Username = TomahawkSettings::instance()->lastFmUsername();
        m_pw = TomahawkSettings::instance()->lastFmPassword();

        createScrobbler();
    }
    else if( m_scrobbler && !TomahawkSettings::instance()->scrobblingEnabled() )
    {
        delete m_scrobbler;
        m_scrobbler = 0;
    }
    else if( TomahawkSettings::instance()->lastFmUsername() != lastfm::ws::Username ||
               TomahawkSettings::instance()->lastFmPassword() != m_pw )
    {
        lastfm::ws::Username = TomahawkSettings::instance()->lastFmUsername();
        m_pw = TomahawkSettings::instance()->lastFmPassword();
        // credentials have changed, have to re-create scrobbler for them to take effect
        if( m_scrobbler )
        {
            delete m_scrobbler;
            m_scrobbler = 0;
        }

        createScrobbler();
    }
}


void
LastFmPlugin::onAuthenticated()
{
    QNetworkReply* authJob = dynamic_cast<QNetworkReply*>( sender() );
    if( !authJob )
    {
        tLog() << Q_FUNC_INFO << "Help! No longer got a last.fm auth job!";
        return;
    }

    if( authJob->error() == QNetworkReply::NoError )
    {
        lastfm::XmlQuery lfm = lastfm::XmlQuery( authJob->readAll() );

        if( lfm.children( "error" ).size() > 0 )
        {
            tLog() << "Error from authenticating with Last.fm service:" << lfm.text();
            TomahawkSettings::instance()->setLastFmSessionKey( QByteArray() );
        }
        else
        {
            lastfm::ws::SessionKey = lfm[ "session" ][ "key" ].text();

            TomahawkSettings::instance()->setLastFmSessionKey( lastfm::ws::SessionKey.toLatin1() );

//            qDebug() << "Got session key from last.fm";
            if( TomahawkSettings::instance()->scrobblingEnabled() )
                m_scrobbler = new lastfm::Audioscrobbler( "thk" );
        }
    }
    else
    {
        tLog() << "Got error in Last.fm authentication job:" << authJob->errorString();
    }

    authJob->deleteLater();
}


void
LastFmPlugin::createScrobbler()
{
    if( TomahawkSettings::instance()->lastFmSessionKey().isEmpty() ) // no session key, so get one
    {
        qDebug() << "LastFmPlugin::createScrobbler Session key is empty";
        QString authToken = md5( ( lastfm::ws::Username.toLower() + md5( m_pw.toUtf8() ) ).toUtf8() );

        QMap<QString, QString> query;
        query[ "method" ] = "auth.getMobileSession";
        query[ "username" ] = lastfm::ws::Username;
        query[ "authToken" ] = authToken;
        QNetworkReply* authJob = lastfm::ws::post( query );

        connect( authJob, SIGNAL( finished() ), SLOT( onAuthenticated() ) );
    }
    else
    {
        qDebug() << "LastFmPlugin::createScrobbler Already have session key";
        lastfm::ws::SessionKey = TomahawkSettings::instance()->lastFmSessionKey();

        m_scrobbler = new lastfm::Audioscrobbler( "thk" );
    }
}


QList<lastfm::Track>
LastFmPlugin::parseTrackList( QNetworkReply* reply )
{
    QList<lastfm::Track> tracks;
    try {
        lastfm::XmlQuery lfm = reply->readAll();
        foreach ( lastfm::XmlQuery xq, lfm.children( "track" ) )
        {
            tracks.append( lastfm::Track( xq ) );
        }
    }
    catch( lastfm::ws::ParseError& e )
    {
        qWarning() << e.what();
    }

    return tracks;
}

