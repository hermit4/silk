import Silk.Config 0.1

Config {
    listen: Listen {
        address: 'localhost'
        port: 18080
    }

    daemons: [
        ':/app/service/carpet.qml'
    ]

    contents: {
        '/': ':/app/web/'
    }

    cache: {
        'qml': true
    }

    // what to back up and where to; the user's home and ~/.carpet when not given
    // property var carpet: { 'from': '/home/user', 'to': '/home/user/.carpet' }
}
