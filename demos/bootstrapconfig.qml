// silk --config demos/bootstrapconfig.qml (outside of the directory it serves)
import Silk.Config 0.1

Config {
    listen: Listen {
        address: '*'
        port: 8081
    }

    contents: {
        '/': 'bootstrap/'
    }

    cache: {
        'qml': false
    }
}
