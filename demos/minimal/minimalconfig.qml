import Silk.Config 0.1

Config {
    listen: Listen {
        address: 'localhost'
        port: 8080
    }

    contents: {
        '/': ':/contents/'
    }

    cache: {
        'qml': true
    }
}
