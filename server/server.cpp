
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <math.h>


#include "minorGems/util/stringUtils.h"
#include "minorGems/util/SettingsManager.h"
#include "minorGems/util/SimpleVector.h"
#include "minorGems/network/SocketServer.h"
#include "minorGems/network/SocketPoll.h"

#include "minorGems/system/Thread.h"

#include "minorGems/game/doublePair.h"


#include "map.h"
#include "../gameSource/transitionBank.h"

#include "minorGems/util/random/JenkinsRandomSource.h"


static JenkinsRandomSource testRandSource;



typedef struct LiveObject {
        int id;
        
        // start and dest for a move
        // same if reached destination
        int xs;
        int ys;
        
        int xd;
        int yd;
        
        int lastSentMapX;
        int lastSentMapY;

        // in grid square widths per second
        double moveSpeed;
        
        double moveTotalSeconds;
        double moveStartTime;
        

        int holdingID;

        Socket *sock;
        SimpleVector<char> *sockBuffer;

        char isNew;
        char firstMessageSent;
        char error;
        char deleteSent;

        char newMove;
        
    } LiveObject;



SimpleVector<LiveObject> players;


int nextID = 0;


volatile char quit = false;

void intHandler( int inUnused ) {
    quit = true;
    }


int numConnections = 0;







// reads all waiting data from socket and stores it in buffer
// returns true if socket still good, false on error
char readSocketFull( Socket *inSock, SimpleVector<char> *inBuffer ) {

    char buffer[512];
    
    int numRead = inSock->receive( (unsigned char*)buffer, 512, 0 );
    
    if( numRead == -1 ) {
        return false;
        }
    
    while( numRead > 0 ) {
        inBuffer->appendArray( buffer, numRead );

        numRead = inSock->receive( (unsigned char*)buffer, 512, 0 );
        }

    return true;
    }



// NULL if there's no full message available
char *getNextClientMessage( SimpleVector<char> *inBuffer ) {
    // find first terminal character #

    int index = inBuffer->getElementIndex( '#' );
        
    if( index == -1 ) {
        return NULL;
        }
    
    char *message = new char[ index + 1 ];
    
    for( int i=0; i<index; i++ ) {
        message[i] = inBuffer->getElementDirect( 0 );
        inBuffer->deleteElement( 0 );
        }
    // delete message terminal character
    inBuffer->deleteElement( 0 );
    
    message[ index ] = '\0';
    
    return message;
    }





typedef enum messageType {
	MOVE,
    USE,
    GRAB,
    DROP,
    UNKNOWN
    } messageType;


typedef struct ClientMessage {
        messageType type;
        int x, y;
    } ClientMessage;




ClientMessage parseMessage( char *inMessage ) {
    
    char nameBuffer[100];
    
    ClientMessage m;
    
    int numRead = sscanf( inMessage, 
                          "%99s %d %d#", nameBuffer, &( m.x ), &( m.y ) );


    if( numRead != 3 ) {
        m.type = UNKNOWN;
        return m;
        }
    

    if( strcmp( nameBuffer, "MOVE" ) == 0) {
        m.type = MOVE;
        }
    else if( strcmp( nameBuffer, "USE" ) == 0 ) {
        m.type = USE;
        }
    else if( strcmp( nameBuffer, "GRAB" ) == 0 ) {
        m.type = GRAB;
        }
    else if( strcmp( nameBuffer, "DROP" ) == 0 ) {
        m.type = DROP;
        }
    else {
        m.type = UNKNOWN;
        }
    
    return m;
    }




// returns NULL if there are no matching moves
char *getMovesMessage( char inNewMovesOnly ) {
    
    SimpleVector<char> messageBuffer;

    messageBuffer.appendElementString( "PLAYER_MOVES_START\n" );

    int numPlayers = players.size();
                
    
    int numLines = 0;

    for( int i=0; i<numPlayers; i++ ) {
                
        LiveObject *o = players.getElement( i );
                

        if( ( o->xd != o->xs || o->yd != o->ys )
            &&
            ( o->newMove || !inNewMovesOnly ) ) {

 
            // p_id xs ys xd yd fraction_done eta_sec
            
            double deltaSec = Time::getCurrentTime() - o->moveStartTime;
            
            double etaSec = o->moveTotalSeconds - deltaSec;
                
            if( inNewMovesOnly ) {
                o->newMove = false;
                }
            
            // holding no object for now
            char *messageLine = 
                autoSprintf( "%d %d %d %d %d %f %f\n", o->id, 
                             o->xs, o->ys, o->xd, o->yd, 
                             o->moveTotalSeconds, etaSec );
                                    
            messageBuffer.appendElementString( messageLine );
            delete [] messageLine;

            numLines ++;
            
            }
        }
    
        
    if( numLines > 0 ) {
        
        messageBuffer.push_back( '#' );
                
        char *message = messageBuffer.getElementString();
        
        return message;
        }
    
    return NULL;
    
    }



static char isGridAdjacent( int inXA, int inYA, int inXB, int inYB ) {
    if( ( abs( inXA - inXB ) == 1 && inYA == inYB ) 
        ||
        ( abs( inYA - inYB ) == 1 && inXA == inXB ) ) {
        
        return true;
        }

    return false;
    }




// sets lastSentMap in inO if chunk goes through
// returns result of send, auto-marks error in inO
int sendMapChunkMessage( LiveObject *inO ) {
    char *mapChunkMessage = getChunkMessage( inO->xs,
                                             inO->ys );
                
                

                
    int messageLength = strlen( mapChunkMessage );

    int numSent = 
        inO->sock->send( (unsigned char*)mapChunkMessage, 
                         messageLength, 
                         false, false );
                
    delete [] mapChunkMessage;
                

    if( numSent == messageLength ) {
        // sent correctly
        inO->lastSentMapX = inO->xs;
        inO->lastSentMapY = inO->ys;
        }
    else if( numSent == -1 ) {
        inO->error = true;
        }
    return numSent;
    }






int main() {

    printf( "Test server\n" );

    signal( SIGINT, intHandler );


    initTransBank();

    initMap();
    
    
    int port = 
        SettingsManager::getIntSetting( "port", 5077 );
    
    
    SocketPoll sockPoll;
    
    
    
    SocketServer server( port, 256 );
    
    sockPoll.addSocketServer( &server );
    
    printf( "Listening for connection on port %d\n", port );

    while( !quit ) {
    
        int numLive = players.size();
        

        // check if any are still moving
        // if so, we must busy-loop over them until moves are
        // complete
        char anyMoving = false;
        double minMoveTime = 999999;
        
        for( int i=0; i<numLive; i++ ) {
            LiveObject *nextPlayer = players.getElement( i );
            
            if( nextPlayer->xd != nextPlayer->xs ||
                nextPlayer->yd != nextPlayer->ys ) {
                
                double moveTimeLeft =
                    nextPlayer->moveTotalSeconds -
                    ( Time::getCurrentTime() - nextPlayer->moveStartTime );
                
                if( moveTimeLeft < 0 ) {
                    moveTimeLeft = 0;
                    }
                
                if( moveTimeLeft < minMoveTime ) {
                    minMoveTime = moveTimeLeft;
                    }
                anyMoving = true;
                }
            }
        
        SocketOrServer *readySock =  NULL;

        if( !anyMoving ) {
            // use 0 cpu when total idle
            readySock = sockPoll.wait();
            }
        else {
            // players are connected and moving, must do move updates anyway
            
            if( minMoveTime > 0 ) {
                
                // use a timeout based on shortest time to complete move
                // so we'll wake up and catch it
                readySock = sockPoll.wait( (int)( minMoveTime * 1000 ) );
                }
            }    
        
        
        
        
        if( readySock != NULL && !readySock->isSocket ) {
            // server ready
            Socket *sock = server.acceptConnection( 0 );

            if( sock != NULL ) {
                
                
                printf( "Got connection\n" );
                numConnections ++;
                
                LiveObject newObject;
                newObject.id = nextID;
                nextID++;
                newObject.xs = 0;
                newObject.ys = 0;
                newObject.xd = 0;
                newObject.yd = 0;
                newObject.lastSentMapX = 0;
                newObject.lastSentMapY = 0;
                newObject.moveSpeed = 4;
                newObject.moveTotalSeconds = 0;
                newObject.holdingID = 0;
                newObject.sock = sock;
                newObject.sockBuffer = new SimpleVector<char>();
                newObject.isNew = true;
                newObject.firstMessageSent = false;
                newObject.error = false;
                newObject.deleteSent = false;
                newObject.newMove = false;

                sockPoll.addSocket( sock );
                
                players.push_back( newObject );            
            
                printf( "New player connected as player %d\n", newObject.id );

                printf( "Listening for another connection on port %d\n", 
                        port );
                }
            }
        
        

        numLive = players.size();
        

        // listen for any messages from clients 

        // track index of each player that needs an update sent about it
        // we compose the full update message below
        SimpleVector<int> playerIndicesToSendUpdatesAbout;
        
        // accumulated text of update lines
        SimpleVector<char> newUpdates;
        

        SimpleVector<char> mapChanges;
        
        
        for( int i=0; i<numLive; i++ ) {
            LiveObject *nextPlayer = players.getElement( i );
            

            char result = 
                readSocketFull( nextPlayer->sock, nextPlayer->sockBuffer );
            
            if( ! result ) {
                nextPlayer->error = true;
                }

            char *message = getNextClientMessage( nextPlayer->sockBuffer );
            
            if( message != NULL ) {
                printf( "Got client message: %s\n", message );
                
                ClientMessage m = parseMessage( message );
                
                delete [] message;
                
                //Thread::staticSleep( 
                //    testRandSource.getRandomBoundedInt( 0, 450 ) );
                
                // if player is still moving, ignore all actions
                // except for move interrupts
                if( ( nextPlayer->xs == nextPlayer->xd &&
                      nextPlayer->ys == nextPlayer->yd ) 
                    ||
                    m.type == MOVE ) {
                    
                    if( m.type == MOVE ) {
                        if( nextPlayer->xs != nextPlayer->xd ||
                            nextPlayer->ys != nextPlayer->yd ) {
                    
                            // a new move interrupting a non-stationary object
                            
                            // compute closest starting position part way along
                            // path
                            double fractionDone = 
                                ( Time::getCurrentTime() - 
                                  nextPlayer->moveStartTime )
                                / nextPlayer->moveTotalSeconds;
                    
                            doublePair start = { nextPlayer->xs, 
                                                 nextPlayer->ys };
                            doublePair dest = { nextPlayer->xd, 
                                                nextPlayer->yd };
                            
                            doublePair cur =
                                add( mult( dest, fractionDone ),
                                     mult( start, 1 - fractionDone ) );
                            
                            nextPlayer->xs = lrintf( cur.x );
                            nextPlayer->ys = lrintf( cur.y );
                            }


                        nextPlayer->xd = m.x;
                        nextPlayer->yd = m.y;
                        
                        doublePair start = { nextPlayer->xs, 
                                             nextPlayer->ys };
                        doublePair dest = { m.x, m.y };
                        
                        double dist = distance( start, dest );
                        
                        
                        nextPlayer->moveTotalSeconds = dist / 
                            nextPlayer->moveSpeed;

                        nextPlayer->moveStartTime = Time::getCurrentTime();
                        
                        nextPlayer->newMove = true;
                        }
                    else if( m.type == USE ) {
                        // send update even if action fails (to let them
                        // know that action is over)
                        playerIndicesToSendUpdatesAbout.push_back( i );

                        if( isGridAdjacent( m.x, m.y,
                                            nextPlayer->xd, 
                                            nextPlayer->yd ) ) {
                            
                            // can only use on targets next to us for now,
                            // no diags
                            
                            int target = getMapObject( m.x, m.y );
                            
                            if( target != 0 ) {
                                
                                TransRecord *r = 
                                    getTrans( nextPlayer->holdingID, 
                                              target );

                                if( r != NULL ) {
                                    nextPlayer->holdingID = r->newActor;
                                    
                                    setMapObject( m.x, m.y, r->newTarget );
                                    
                                    char *changeLine =
                                        autoSprintf( "%d %d %d\n",
                                                     m.x, m.y, r->newTarget );
                                
                                    mapChanges.
                                        appendElementString( changeLine );
                                    
                                    delete [] changeLine;
                                    }
                                else if( nextPlayer->holdingID == 0 ) {
                                    // no bare-hand transition applies to
                                    // this target object
                                    // treat it like GRAB
                                    setMapObject( m.x, m.y, 0 );
                                    
                                    nextPlayer->holdingID = target;
                                
                                
                                    char *changeLine =
                                        autoSprintf( "%d %d %d\n",
                                                     m.x, m.y, 0 );
                                
                                    mapChanges.appendElementString( 
                                        changeLine );
                                    
                                    delete [] changeLine;
                                    }    
                                }
                            }
                        }                    
                    else if( m.type == DROP ) {
                        // send update even if action fails (to let them
                        // know that action is over)
                        playerIndicesToSendUpdatesAbout.push_back( i );

                        if( isGridAdjacent( m.x, m.y,
                                            nextPlayer->xd, 
                                            nextPlayer->yd ) ) {
                            
                            int target = getMapObject( m.x, m.y );
                            
                            if( nextPlayer->holdingID != 0 && 
                                target == 0 ) {
                                
                                // empty spot to drop into
                                    
                                setMapObject( m.x, m.y, 
                                              nextPlayer->holdingID );
                                
                                char *changeLine =
                                    autoSprintf( "%d %d %d\n",
                                                 m.x, m.y,
                                                 nextPlayer->holdingID );
                                
                                mapChanges.appendElementString( 
                                    changeLine );
                                
                                delete [] changeLine;
                                
                                nextPlayer->holdingID = 0;
                                }
                            }
                        }
                    else if( m.type == GRAB ) {
                        // send update even if action fails (to let them
                        // know that action is over)
                        playerIndicesToSendUpdatesAbout.push_back( i );

                        if( isGridAdjacent( m.x, m.y,
                                            nextPlayer->xd, 
                                            nextPlayer->yd ) ) {
                            
                            int target = getMapObject( m.x, m.y );
                            
                            if( nextPlayer->holdingID == 0 && 
                                target != 0 ) {
                                
                                // something to grab

                                setMapObject( m.x, m.y, 0 );
                                
                                nextPlayer->holdingID = target;
                                
                                char *changeLine =
                                    autoSprintf( "%d %d %d\n",
                                                 m.x, m.y, 0 );
                                
                                mapChanges.appendElementString( 
                                    changeLine );
                                
                                delete [] changeLine;
                                }
                            }
                        }
                    }                
                }
            
                
            if( nextPlayer->isNew ) {
                // their first position is an update
                

                playerIndicesToSendUpdatesAbout.push_back( i );
                
                nextPlayer->isNew = false;
                }
            else if( nextPlayer->error && ! nextPlayer->deleteSent ) {
                char *updateLine = autoSprintf( "%d %d X X %f\n", 
                                                nextPlayer->id,
                                                nextPlayer->holdingID,
                                                nextPlayer->moveSpeed );
                
                newUpdates.appendElementString( updateLine );
                
                delete [] updateLine;
                
                nextPlayer->isNew = false;
                
                nextPlayer->deleteSent = true;
                }
            else {
                // check if they are done moving
                // if so, send an update
                

                if( nextPlayer->xd != nextPlayer->xs ||
                    nextPlayer->yd != nextPlayer->ys ) {
                
                    
                    if( Time::getCurrentTime() - nextPlayer->moveStartTime
                        >
                        nextPlayer->moveTotalSeconds ) {
                        
                        // done
                        nextPlayer->xs = nextPlayer->xd;
                        nextPlayer->ys = nextPlayer->yd;
                        nextPlayer->newMove = false;
                        

                        playerIndicesToSendUpdatesAbout.push_back( i );
                        }
                    }
                
                }
            
            
            }
        

        
        

        
        for( int i=0; i<playerIndicesToSendUpdatesAbout.size(); i++ ) {
            LiveObject *nextPlayer = players.getElement( 
                playerIndicesToSendUpdatesAbout.getElementDirect( i ) );

            char *updateLine = autoSprintf( "%d %d %d %d %f\n", 
                                            nextPlayer->id,
                                            nextPlayer->holdingID,
                                            nextPlayer->xs, 
                                            nextPlayer->ys,
                                            nextPlayer->moveSpeed );
            
            newUpdates.appendElementString( updateLine );
            
            delete [] updateLine;
            }
        

        
        char *moveMessage = getMovesMessage( true );
        
        int moveMessageLength = 0;
        
        if( moveMessage != NULL ) {
            moveMessageLength = strlen( moveMessage );
            }
        
                



        char *updateMessage = NULL;
        int updateMessageLength = 0;
        
        if( newUpdates.size() > 0 ) {
            newUpdates.push_back( '#' );
            char *temp = newUpdates.getElementString();

            updateMessage = concatonate( "PLAYER_UPDATE\n", temp );
            delete [] temp;

            updateMessageLength = strlen( updateMessage );
            }
        

        char *mapChangeMessage = NULL;
        int mapChangeMessageLength = 0;
        
        if( mapChanges.size() > 0 ) {
            mapChanges.push_back( '#' );
            char *temp = mapChanges.getElementString();

            mapChangeMessage = concatonate( "MAP_CHANGE\n", temp );
            delete [] temp;

            mapChangeMessageLength = strlen( mapChangeMessage );
            }
        

        
        // send moves and updates to clients
        
        for( int i=0; i<numLive; i++ ) {
            
            LiveObject *nextPlayer = players.getElement(i);
            
            
            if( ! nextPlayer->firstMessageSent ) {
                

                // first, send the map chunk around them
                
                int numSent = sendMapChunkMessage( nextPlayer );
                
                if( numSent == -2 ) {
                    // still not sent, try again later
                    continue;
                    }



                // now send starting message
                SimpleVector<char> messageBuffer;

                messageBuffer.appendElementString( "PLAYER_UPDATE\n" );

                int numPlayers = players.size();
            
                // must be last in message
                char *playersLine;
                
                for( int i=0; i<numPlayers; i++ ) {
                
                    LiveObject o = *( players.getElement( i ) );
                

                    // holding no object for now
                    char *messageLine = 
                        autoSprintf( "%d %d %d %d %f\n", o.id, o.holdingID,
                                     o.xs, o.ys, o.moveSpeed );
                    

                    if( o.id != nextPlayer->id ) {
                        messageBuffer.appendElementString( messageLine );
                        delete [] messageLine;
                        }
                    else {
                        // save until end
                        playersLine = messageLine;
                        }
                    }

                messageBuffer.appendElementString( playersLine );
                delete [] playersLine;
                
                messageBuffer.push_back( '#' );
            
                char *message = messageBuffer.getElementString();
                int messageLength = strlen( message );

                numSent = 
                    nextPlayer->sock->send( (unsigned char*)message, 
                                            messageLength, 
                                            false, false );
                
                delete [] message;
                

                if( numSent == -1 ) {
                    nextPlayer->error = true;
                    }
                else if( numSent != messageLength ) {
                    // still not sent, try again later
                    continue;
                    }



                char *movesMessage = getMovesMessage( false );
                
                if( movesMessage != NULL ) {
                    
                
                    messageLength = strlen( movesMessage );
                    
                    numSent = 
                        nextPlayer->sock->send( (unsigned char*)movesMessage, 
                                                messageLength, 
                                            false, false );
                    
                    delete [] movesMessage;
                    

                    if( numSent == -1 ) {
                        nextPlayer->error = true;
                        }
                    else if( numSent != messageLength ) {
                        // still not sent, try again later
                        continue;
                        }
                    }
                
                nextPlayer->firstMessageSent = true;
                }
            else {
                // this player has first message, ready for updates/moves
                
                if( abs( nextPlayer->xd - nextPlayer->lastSentMapX ) > 10
                    ||
                    abs( nextPlayer->yd - nextPlayer->lastSentMapY ) > 10 ) {
                
                    // moving out of bounds of chunk, send update
                    
                    
                    sendMapChunkMessage( nextPlayer );
                    }
                

                if( updateMessage != NULL ) {
                    

                    int numSent = 
                        nextPlayer->sock->send( (unsigned char*)updateMessage, 
                                                updateMessageLength, 
                                                false, false );

                    if( numSent == -1 ) {
                        nextPlayer->error = true;
                        }
                    }
                if( moveMessage != NULL ) {
                    int numSent = 
                        nextPlayer->sock->send( (unsigned char*)moveMessage, 
                                                moveMessageLength, 
                                                false, false );

                    if( numSent == -1 ) {
                        nextPlayer->error = true;
                        }
                    
                    }
                if( mapChangeMessage != NULL ) {
                    int numSent = 
                        nextPlayer->sock->send( 
                            (unsigned char*)mapChangeMessage, 
                            mapChangeMessageLength, 
                            false, false );

                    if( numSent == -1 ) {
                        nextPlayer->error = true;
                        }
                    
                    }
                
                }
            }

        if( moveMessage != NULL ) {
            delete [] moveMessage;
            }
        if( updateMessage != NULL ) {
            delete [] updateMessage;
            }
        if( mapChangeMessage != NULL ) {
            delete [] mapChangeMessage;
            }
        

        
        // handle closing any that have an error
        for( int i=0; i<players.size(); i++ ) {
            LiveObject *nextPlayer = players.getElement(i);

            if( nextPlayer->error && nextPlayer->deleteSent ) {
                printf( "Closing connection to player %d on error\n",
                        nextPlayer->id );
                
                delete nextPlayer->sock;
                delete nextPlayer->sockBuffer;
                players.deleteElement( i );
                i--;
                }
            }

        }
    

    printf( "Quitting...\n" );
    

    for( int i=0; i<players.size(); i++ ) {
        LiveObject *nextPlayer = players.getElement(i);
        delete nextPlayer->sock;
        delete nextPlayer->sockBuffer;
        }
    
    freeMap();

    freeTransBank();

    printf( "Done.\n" );


    return 0;
    }
