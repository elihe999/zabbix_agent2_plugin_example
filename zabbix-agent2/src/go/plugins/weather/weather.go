package weather

import (
	"errors"
	"fmt"
	"io/ioutil"
	"net/http"
	"net/url"
	"time"
	"zabbix.com/pkg/conf"
	"zabbix.com/pkg/plugin"
)

type Plugin struct {
	plugin.Base
	options PluginOptions
	httpClient http.Client
}

var impl Plugin

type PluginOptions struct {
	// Timeout is the maximum time for waiting when a request has to be done. Default value equals the global timeout.
	Timeout int `conf:"optional,range=1:30"`
}

func (p *Plugin) Export(key string, params []string, ctx plugin.ContextProvider) (result interface{}, err error) {
	if len(params) != 1 {
		return nil, errors.New("Wrong parameters.")
	}

	// https://github.com/chubin/wttr.in
	res, err := p.httpClient.Get(fmt.Sprintf("https://wttr.in/~%s?format=%%t", params[0]))
	if err != nil {
		if err.(*url.Error).Timeout() {
			return nil, errors.New("Request timeout.")
		}
		return nil, err
	}

	temp, err := ioutil.ReadAll(res.Body)
	_ = res.Body.Close()
	if err != nil {
		return nil, err
	}

	return string(temp)[0 : len(temp)-4], nil
}

func (p *Plugin) Configure(global *plugin.GlobalOptions, privateOptions interface{}) {
	if err := conf.Unmarshal(privateOptions, &p.options); err != nil {
		p.Errf("cannot unmarshal configuration options: %s", err)
	}

	// Set default value
	if p.options.Timeout == 0 {
		p.options.Timeout = global.Timeout
	}

	p.httpClient = http.Client{Timeout: time.Duration(p.options.Timeout) * time.Second}
}

func (p *Plugin) Validate(privateOptions interface{}) error {
	var opts PluginOptions
	return conf.Unmarshal(privateOptions, &opts)
}

func init() {
	plugin.RegisterMetrics(&impl, "Weather", "weather.temp", "Returns Celsius temperature.")
}