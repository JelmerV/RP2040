/*
  ioports_analog.c - driver code for RP2040 ARM processors

  Part of grblHAL

  Copyright (c) 2023-2024 Terje Io

  Grbl is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Grbl is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Grbl.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "driver.h"

#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"
#include "hardware/adc.h"

#include "grbl/ioports.h"

#define ADC_TO_GPIO_SHIFT 26            // GPIO26 is ADC0
#define ADC_CONVERSION_FACTOR 3.3f / (1 << 12)  // 12-bit ADC



#ifdef MCP3221_ENABLE
#include "MCP3221.h"
static xbar_t analog_in;
static enumerate_pins_ptr on_enumerate_pins;
static void enumerate_pins (bool low_level, pin_info_ptr pin_info, void *data)
{
    on_enumerate_pins(low_level, pin_info, data);

    pin_info(&analog_in, data);
}
#endif // MCP3221_ENABLE

static io_ports_data_t analog;
static input_signal_t *aux_in_analog;
static output_signal_t *aux_out_analog;
static ioports_pwm_t *pwm_data;
static float *pwm_values;

static wait_on_input_ptr wait_on_input_digital;
static set_pin_description_ptr set_pin_description_digital;
static get_pin_info_ptr get_pin_info_digital;
static claim_port_ptr claim_digital;
static swap_pins_ptr swap_pins_digital; 

static void set_pwm_cap (xbar_t *output, bool servo_pwm)
{
    uint_fast8_t i = analog.out.n_ports;

    if(output) do {
        i--;
        if(aux_out_analog[i].pin == output->pin) {
            aux_out_analog[i].mode.pwm = !servo_pwm;
            aux_out_analog[i].mode.servo_pwm = servo_pwm;
            break;
        }
    } while(i);
}

static bool init_pwm (xbar_t *output, pwm_config_t *config)
{
    bool ok;
    ioports_pwm_t *pwm_data = (ioports_pwm_t *)output->port;
    uint32_t prescaler = config->freq_hz > 2000.0f ? 1 : (config->freq_hz > 200.0f ? 12 : 50);

    if((ok = ioports_precompute_pwm_values(config, pwm_data, clock_get_hz(clk_sys) / prescaler))) {

        pwm_config pwm_config = pwm_get_default_config();
        pwm_config_set_clkdiv_int(&pwm_config, prescaler);
        pwm_config_set_wrap(&pwm_config, pwm_data->period);

        gpio_set_function(output->pin, GPIO_FUNC_PWM);
        pwm_set_gpio_level(output->pin, pwm_data->off_value);

        uint channel = pwm_gpio_to_channel(output->pin);
        pwm_config_set_output_polarity(&pwm_config, (!channel & config->invert), (channel & config->invert));

        pwm_init(pwm_gpio_to_slice_num(output->pin), &pwm_config, true);

        set_pwm_cap(output, config->servo_mode);
    }
    
    return ok;
}

static float pwm_get_value (struct xbar *output)
{
    int_fast8_t ch = output->function - Output_Analog_Aux0;

    return pwm_values && ch >= 0 && ch < analog.out.n_ports ? pwm_values[ch] : -1.0f;
}

static bool analog_out (uint8_t port, float value)
{
    if(port < analog.out.n_ports) {
        port = ioports_map(analog.out, port);
        if(pwm_values)
            pwm_values[aux_out_analog[port].id - Output_Analog_Aux0] = value;
        pwm_set_gpio_level(aux_out_analog[port].pin, ioports_compute_pwm_value(&pwm_data[aux_out_analog[port].pwm_idx], value));
    }

    return port < analog.out.n_ports;
}



static int32_t wait_on_input_dummy (io_port_type_t type, uint8_t port, wait_mode_t wait_mode, float timeout)
{
    return -1;
}

static int32_t wait_on_input (io_port_type_t type, uint8_t port, wait_mode_t wait_mode, float timeout)
{
    int32_t value = -1;

    if(type == Port_Digital)
        return wait_on_input_digital(type, port, wait_mode, timeout);

    port = ioports_map(analog.in, port);

#ifdef MCP3221_ENABLE
    if(port == analog_in.pin)
        value = (int32_t)MCP3221_read();
    else
#endif
    if(port < analog.in.n_ports) {
        adc_select_input(aux_in_analog[port].pin - ADC_TO_GPIO_SHIFT);
        value = adc_read();
    }

    return value;
}


static xbar_t *get_pin_info (io_port_type_t type, io_port_direction_t dir, uint8_t port)
{
    static xbar_t pin;
    xbar_t *info = NULL;

    if(type == Port_Digital)
        return get_pin_info_digital ? get_pin_info_digital(type, dir, port) : NULL;

    else if(dir == Port_Input && port < analog.in.n_ports) {
        port = ioports_map(analog.in, port);
#ifdef MCP3221_ENABLE
        if(port == analog_in.pin)
            info = &analog_in;
        else
#endif
        {
            pin.mode = aux_in_analog[port].mode;
            pin.cap = aux_in_analog[port].cap;
            pin.cap.claimable = !pin.mode.claimed;
            pin.function = aux_in_analog[port].id;
            pin.group = aux_in_analog[port].group;
            pin.pin = aux_in_analog[port].pin;
            pin.bit = 1 << aux_in_analog[port].pin;
            pin.port = (void *)aux_in_analog[port].port;
            pin.description = aux_in_analog[port].description;
            info = &pin;
        }
    }

    else if(dir == Port_Output && port < analog.out.n_ports) {
        memset(&pin, 0, sizeof(xbar_t));

        port = ioports_map(analog.out, port);
        pin.mode = aux_out_analog[port].mode;
        pin.mode.pwm = !pin.mode.servo_pwm; //?? for easy filtering
        XBAR_SET_CAP(pin.cap, pin.mode);
        pin.function = aux_out_analog[port].id;
        pin.group = aux_out_analog[port].group;
        pin.pin = aux_out_analog[port].pin;
        pin.bit = 1 << aux_out_analog[port].pin;
        pin.description = aux_out_analog[port].description;
        if(aux_out_analog[port].mode.pwm || aux_out_analog[port].mode.servo_pwm) {
            pin.port = &pwm_data[aux_out_analog[port].pwm_idx];
            pin.config = (xbar_config_ptr)init_pwm;
            pin.get_value = pwm_get_value;
        }
        info = &pin;
    }

    return info;
}

static void set_pin_description (io_port_type_t type, io_port_direction_t dir, uint8_t port, const char *description)
{
    if(type == Port_Analog) {
        if(dir == Port_Input && port < analog.in.n_ports) {
            port = ioports_map(analog.in, port);
#ifdef MCP3221_ENABLE
            if(port == analog_in.pin)
                analog_in.description = description;
            else
#endif
            aux_in_analog[port].description = description;
        }
        else if(dir == Port_Output && port < analog.out.n_ports) {
            aux_out_analog[ioports_map(analog.out, port)].description = description;
        }
    } 
    else if(set_pin_description_digital) {
        set_pin_description_digital(type, dir, port, description);
    }
}

static bool claim (io_port_type_t type, io_port_direction_t dir, uint8_t *port, const char *description)
{
    bool ok = false;

    if(type == Port_Digital) {
        return claim_digital ? claim_digital(type, dir, port, description) : false;
    }

    else if(dir == Port_Input) {
        if((ok = analog.in.map && *port < analog.in.n_ports && !(
#ifdef MCP3221_ENABLE
            *port == analog_in.pin ? analog_in.mode.claimed :
#endif
            aux_in_analog[*port].mode.claimed))) {

            uint8_t i;

            hal.port.num_analog_in--;

            for(i = ioports_map_reverse(&analog.in, *port); i < hal.port.num_analog_in; i++) {
                analog.in.map[i] = analog.in.map[i + 1];
#ifdef MCP3221_ENABLE
                if(analog_in.pin == analog.in.map[i])
                    analog_in.description = iports_get_pnum(analog, i);
                else
#endif
                aux_in_analog[analog.in.map[i]].description = iports_get_pnum(analog, i);
            }

#ifdef MCP3221_ENABLE
            if(*port == analog_in.pin) {
                analog_in.mode.claimed = On;
                analog_in.description = description;
            } else
#endif
            {
                aux_in_analog[*port].mode.claimed = On;
                aux_in_analog[*port].description = description;
            }
            analog.in.map[hal.port.num_analog_in] = *port;
            *port = hal.port.num_analog_in;
        }
    }
    else if(dir == Port_Output) {
        if((ok = analog.out.map && *port < analog.out.n_ports && !aux_out_analog[*port].mode.claimed)) {

            uint8_t i;

            hal.port.num_analog_out--;

            for(i = ioports_map_reverse(&analog.out, *port); i < hal.port.num_analog_out; i++) {
                analog.out.map[i] = analog.out.map[i + 1];
                aux_out_analog[analog.out.map[i]].description = iports_get_pnum(analog, i);
            }

            aux_out_analog[*port].mode.claimed = On;
            aux_out_analog[*port].description = description;

            analog.out.map[hal.port.num_analog_out] = *port;
            *port = hal.port.num_analog_out;
        }
    }

    return ok;
}

void ioports_init_analog (pin_group_pins_t *aux_inputs, pin_group_pins_t *aux_outputs)
{
    uint_fast8_t i;

    aux_in_analog = aux_inputs->pins.inputs;
    aux_out_analog = aux_outputs->pins.outputs;

    set_pin_description_digital = hal.port.set_pin_description;
    hal.port.set_pin_description = set_pin_description;

    
#ifdef MCP3221_ENABLE

    pin_group_pins_t aux_in = {
        .n_pins = 1
    };

    analog_in.function = Input_Analog_Aux0 + aux_inputs->n_pins;
    analog_in.group = PinGroup_AuxInput;
    analog_in.pin = aux_inputs->n_pins;
    analog_in.port = "MCP3221:";

    if(MCP3221_init()) {
        analog_in.mode.analog = On;
        if(aux_inputs)
            aux_inputs->n_pins++;
        else
            aux_inputs = &aux_in;
    } else
        analog_in.description = "No power";

    on_enumerate_pins = hal.enumerate_pins;
    hal.enumerate_pins = enumerate_pins;

#endif // MCP3221_ENABLE

    if(ioports_add(&analog, Port_Analog, aux_inputs->n_pins, aux_outputs->n_pins))  {
        if(analog.in.n_ports) {
            adc_init();  //Initialise the ADC HW. 

            for(i = 0; i < aux_inputs->n_pins; i++) {
                adc_gpio_init(aux_inputs->pins.inputs[i].pin);  // Prepare for ADC usage by disabling all digital functions.
                adc_select_input(aux_inputs->pins.inputs[i].pin - ADC_TO_GPIO_SHIFT);  //Select an ADC input. DAC0-3 are GPIOs 26-29 
            }

            if((wait_on_input_digital = hal.port.wait_on_input) == NULL)
                wait_on_input_digital = wait_on_input_dummy;
            hal.port.wait_on_input = wait_on_input;
        }

        if(analog.out.n_ports) {
            
            uint_fast8_t n_pwm = 0;

            pwm_config_t config = {
                .freq_hz = 5000.0f,
                .min = 0.0f,
                .max = 100.0f,
                .off_value = 0.0f,
                .min_value = 0.0f,
                .max_value = 100.0f,
                .invert = Off
            };

            hal.port.analog_out = analog_out;

            for(i = 0; i < analog.out.n_ports; i++) {
                if(aux_out_analog[i].mode.pwm)
                    n_pwm++;
            }

            pwm_data = calloc(n_pwm, sizeof(ioports_pwm_t));
            pwm_values = calloc(n_pwm, sizeof(float));

            n_pwm = 0;
            for(i = 0; i < analog.out.n_ports; i++) {
                if(aux_out_analog[i].mode.pwm && !!pwm_data) {
                    aux_out_analog[i].pwm_idx = n_pwm++;
                    init_pwm(get_pin_info(Port_Analog, Port_Output, i), &config);
                }
            }
        }

        claim_digital = hal.port.claim;
        hal.port.claim = claim;

        get_pin_info_digital = hal.port.get_pin_info;
        hal.port.get_pin_info = get_pin_info;
 
        // swap_pins_digital = hal.port.swap_pins;
        // hal.port.swap_pins = swap_pins;
    }
}
