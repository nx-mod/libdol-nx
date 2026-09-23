# platform/net

Sockets, for the games that have them.

| File | Is |
|---|---|
| `network_core.cpp`, `network_socket.cpp` | the socket calls a game makes |
| `network_config.cpp` | the connection settings it reads |
| `network_ssl.cpp` | the SSL service the Wii offers titles |
| `network_deferred.cpp` | calls that must not block the thread that made them |

Nothing here reaches Nintendo's servers, which no longer answer; a game talks to
whatever the player's own configuration points it at.
