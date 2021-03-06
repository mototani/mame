// license:BSD-3-Clause
// copyright-holders:hap
// thanks-to:Berger
/******************************************************************************

    Chess King generic Z80 based chess computer driver

    NOTE: MAME doesn't include a generalized implementation for boardpieces yet,
    greatly affecting user playability of emulated electronic board games.
    As workaround for the chess games, use an external chess GUI on the side,
    such as Arena(in editmode).

    TODO:
    - ckmaster 1 WAIT CLK per M1, workaround with z80_set_cycle_tables is possible
      (wait state is similar to MSX) but I can't be bothered, better solution
      is to add M1 pin to the z80 core. Until then, it'll run ~20% too fast.

******************************************************************************

Master:
- Z80 CPU(NEC D780C-1) @ 4MHz(8MHz XTAL), IRQ from 555 timer
- 8KB ROM(NEC D2764C-3), 2KB RAM(NEC D4016C), ROM is scrambled for easy PCB placement
- simple I/O via 2*74373 and a 74145
- 8*8 chessboard buttons, 32+1 border leds, piezo

******************************************************************************/

#include "emu.h"
#include "cpu/z80/z80.h"
#include "machine/timer.h"
#include "machine/bankdev.h"
#include "sound/dac.h"
#include "sound/volt_reg.h"
#include "speaker.h"

// internal artwork
#include "ck_master.lh" // clickable


class ckingbase_state : public driver_device
{
public:
	ckingbase_state(const machine_config &mconfig, device_type type, const char *tag) :
		driver_device(mconfig, type, tag),
		m_maincpu(*this, "maincpu"),
		m_irq_on(*this, "irq_on"),
		m_inp_matrix(*this, "IN.%u", 0),
		m_out_x(*this, "%u.%u", 0U, 0U),
		m_out_a(*this, "%u.a", 0U),
		m_out_digit(*this, "digit%u", 0U),
		m_display_wait(33),
		m_display_maxy(1),
		m_display_maxx(0)
	{ }

	// devices/pointers
	required_device<cpu_device> m_maincpu;
	optional_device<timer_device> m_irq_on;
	optional_ioport_array<10> m_inp_matrix; // max 10
	output_finder<0x20, 0x20> m_out_x;
	output_finder<0x20> m_out_a;
	output_finder<0x20> m_out_digit;

	template<int L> TIMER_DEVICE_CALLBACK_MEMBER(irq_on) { m_maincpu->set_input_line(L, ASSERT_LINE); }
	template<int L> TIMER_DEVICE_CALLBACK_MEMBER(irq_off) { m_maincpu->set_input_line(L, CLEAR_LINE); }

	// misc common
	u16 m_inp_mux;                  // multiplexed keypad mask
	u16 m_led_select;
	u16 m_led_data;

	u16 read_inputs(int columns);

	// display common
	int m_display_wait;             // led/lamp off-delay in milliseconds (default 33ms)
	int m_display_maxy;             // display matrix number of rows
	int m_display_maxx;             // display matrix number of columns (max 31 for now)

	u32 m_display_state[0x20];      // display matrix rows data (last bit is used for always-on)
	u16 m_display_segmask[0x20];    // if not 0, display matrix row is a digit, mask indicates connected segments
	u8 m_display_decay[0x20][0x20]; // (internal use)

	TIMER_DEVICE_CALLBACK_MEMBER(display_decay_tick);
	void display_update();
	void set_display_size(int maxx, int maxy);
	void display_matrix(int maxx, int maxy, u32 setx, u32 sety, bool update = true);

protected:
	virtual void machine_start() override;
	virtual void machine_reset() override;
};


// machine start/reset

void ckingbase_state::machine_start()
{
	// resolve handlers
	m_out_x.resolve();
	m_out_a.resolve();
	m_out_digit.resolve();

	// zerofill
	memset(m_display_state, 0, sizeof(m_display_state));
	memset(m_display_decay, 0, sizeof(m_display_decay));
	memset(m_display_segmask, 0, sizeof(m_display_segmask));

	m_inp_mux = 0;
	m_led_select = 0;
	m_led_data = 0;

	// register for savestates
	save_item(NAME(m_display_maxy));
	save_item(NAME(m_display_maxx));
	save_item(NAME(m_display_wait));

	save_item(NAME(m_display_state));
	save_item(NAME(m_display_decay));
	save_item(NAME(m_display_segmask));

	save_item(NAME(m_inp_mux));
	save_item(NAME(m_led_select));
	save_item(NAME(m_led_data));
}

void ckingbase_state::machine_reset()
{
}



/***************************************************************************

  Helper Functions

***************************************************************************/

// The device may strobe the outputs very fast, it is unnoticeable to the user.
// To prevent flickering here, we need to simulate a decay.

void ckingbase_state::display_update()
{
	for (int y = 0; y < m_display_maxy; y++)
	{
		u32 active_state = 0;

		for (int x = 0; x <= m_display_maxx; x++)
		{
			// turn on powered segments
			if (m_display_state[y] >> x & 1)
				m_display_decay[y][x] = m_display_wait;

			// determine active state
			u32 ds = (m_display_decay[y][x] != 0) ? 1 : 0;
			active_state |= (ds << x);

			// output to y.x, or y.a when always-on
			if (x != m_display_maxx)
				m_out_x[y][x] = ds;
			else
				m_out_a[y] = ds;
		}

		// output to digity
		if (m_display_segmask[y] != 0)
			m_out_digit[y] = active_state & m_display_segmask[y];
	}
}

TIMER_DEVICE_CALLBACK_MEMBER(ckingbase_state::display_decay_tick)
{
	// slowly turn off unpowered segments
	for (int y = 0; y < m_display_maxy; y++)
		for (int x = 0; x <= m_display_maxx; x++)
			if (m_display_decay[y][x] != 0)
				m_display_decay[y][x]--;

	display_update();
}

void ckingbase_state::set_display_size(int maxx, int maxy)
{
	m_display_maxx = maxx;
	m_display_maxy = maxy;
}

void ckingbase_state::display_matrix(int maxx, int maxy, u32 setx, u32 sety, bool update)
{
	set_display_size(maxx, maxy);

	// update current state
	u32 mask = (1 << maxx) - 1;
	for (int y = 0; y < maxy; y++)
		m_display_state[y] = (sety >> y & 1) ? ((setx & mask) | (1 << maxx)) : 0;

	if (update)
		display_update();
}


// generic input handlers

u16 ckingbase_state::read_inputs(int columns)
{
	u16 ret = 0;

	// read selected input rows
	for (int i = 0; i < columns; i++)
		if (m_inp_mux >> i & 1)
			ret |= m_inp_matrix[i]->read();

	return ret;
}

namespace {

class master_state : public ckingbase_state
{
public:
	master_state(const machine_config &mconfig, device_type type, const char *tag) :
		ckingbase_state(mconfig, type, tag),
		m_dac(*this, "dac"),
		m_mainmap(*this, "mainmap")
	{ }

	void master(machine_config &config);

	void init_master();

private:
	// devices/pointers
	required_device<dac_2bit_binary_weighted_ones_complement_device> m_dac;
	required_device<address_map_bank_device> m_mainmap;

	void main_map(address_map &map);
	void main_trampoline(address_map &map);
	u8 main_trampoline_r(offs_t offset);
	void main_trampoline_w(offs_t offset, u8 data);

	// I/O handlers
	u8 input_r();
	void control_w(u8 data);
};

// Devices, I/O

/******************************************************************************
    Master
******************************************************************************/

// TTL/generic

void master_state::control_w(u8 data)
{
	// d0-d3: 74145 A-D
	// 74145 0-9: input mux, led select
	u16 sel = 1 << (data & 0xf) & 0x3ff;
	m_inp_mux = sel;

	// d4,d5: led data
	display_matrix(2, 9, data >> 4 & 3, sel & 0x1ff);

	// d6,d7: speaker +/-
	m_dac->write(data >> 6 & 3);
}

u8 master_state::input_r()
{
	// d0-d7: multiplexed inputs (active low)
	return ~read_inputs(10);
}

void master_state::init_master()
{
	u8 *rom = memregion("maincpu")->base();
	const u32 len = memregion("maincpu")->bytes();

	// descramble data lines
	for (int i = 0; i < len; i++)
		rom[i] = bitswap<8>(rom[i], 4,5,0,7,6,1,3,2);

	// descramble address lines
	std::vector<u8> buf(len);
	memcpy(&buf[0], rom, len);
	for (int i = 0; i < len; i++)
		rom[i] = buf[bitswap<16>(i, 15,14,13,12,11,3,7,9, 10,8,6,5,4,2,1,0)];
}



/******************************************************************************
    Address Maps
******************************************************************************/

// Master

void master_state::main_map(address_map &map)
{
	map(0x0000, 0x1fff).mirror(0x6000).rom().region("maincpu", 0); // _A15
	map(0xa000, 0xa000).mirror(0x1fff).rw(FUNC(master_state::input_r), FUNC(master_state::control_w)); // A13
	map(0xc000, 0xc7ff).mirror(0x3800).ram(); // A14
}

// PCB design is prone to bus conflicts, but should be fine if software obeys
void master_state::main_trampoline_w(offs_t offset, u8 data)
{
	if (offset & 0x2000)
		m_mainmap->write8((offset & 0x3fff) | 0x8000, data);
	if (offset & 0x4000)
		m_mainmap->write8((offset & 0x7fff) | 0x8000, data);
}

u8 master_state::main_trampoline_r(offs_t offset)
{
	u8 data = 0xff;
	if (~offset & 0x8000)
		data &= m_mainmap->read8(offset);
	if (offset & 0x2000)
		data &= m_mainmap->read8((offset & 0x3fff) | 0x8000);
	if (offset & 0x4000)
		data &= m_mainmap->read8((offset & 0x7fff) | 0x8000);

	return data;
}

void master_state::main_trampoline(address_map &map)
{
	map(0x0000, 0xffff).rw(FUNC(master_state::main_trampoline_r), FUNC(master_state::main_trampoline_w));
}



/******************************************************************************
    Input Ports
******************************************************************************/

INPUT_PORTS_START( cking_cb_buttons )
	PORT_START("IN.0")
	PORT_BIT(0x01, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x02, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x04, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x08, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x10, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x20, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x40, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x80, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")

	PORT_START("IN.1")
	PORT_BIT(0x01, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x02, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x04, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x08, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x10, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x20, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x40, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x80, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")

	PORT_START("IN.2")
	PORT_BIT(0x01, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x02, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x04, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x08, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x10, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x20, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x40, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x80, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")

	PORT_START("IN.3")
	PORT_BIT(0x01, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x02, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x04, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x08, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x10, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x20, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x40, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x80, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")

	PORT_START("IN.4")
	PORT_BIT(0x01, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x02, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x04, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x08, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x10, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x20, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x40, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x80, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")

	PORT_START("IN.5")
	PORT_BIT(0x01, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x02, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x04, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x08, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x10, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x20, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x40, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x80, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")

	PORT_START("IN.6")
	PORT_BIT(0x01, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x02, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x04, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x08, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x10, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x20, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x40, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x80, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")

	PORT_START("IN.7")
	PORT_BIT(0x01, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x02, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x04, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x08, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x10, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x20, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x40, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
	PORT_BIT(0x80, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_NAME("Board Sensor")
INPUT_PORTS_END


static INPUT_PORTS_START( master )
	PORT_INCLUDE( cking_cb_buttons )

	PORT_START("IN.8")
	PORT_BIT(0x01, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_CODE(KEYCODE_1) PORT_NAME("Change Position")
	PORT_BIT(0x02, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_CODE(KEYCODE_2) PORT_NAME("Clear Board")
	PORT_BIT(0x04, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_CODE(KEYCODE_3) PORT_NAME("New Game")
	PORT_BIT(0x08, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_CODE(KEYCODE_4) PORT_NAME("Take Back")
	PORT_BIT(0x10, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_CODE(KEYCODE_5) PORT_NAME("King")
	PORT_BIT(0x20, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_CODE(KEYCODE_6) PORT_NAME("Queen")
	PORT_BIT(0x40, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_CODE(KEYCODE_7) PORT_NAME("Rook")
	PORT_BIT(0x80, IP_ACTIVE_HIGH, IPT_UNUSED)

	PORT_START("IN.9")
	PORT_BIT(0x01, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_CODE(KEYCODE_8) PORT_NAME("Bishop")
	PORT_BIT(0x02, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_CODE(KEYCODE_9) PORT_NAME("Knight")
	PORT_BIT(0x04, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_CODE(KEYCODE_0) PORT_NAME("Pawn")
	PORT_BIT(0x08, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_CODE(KEYCODE_Q) PORT_NAME("White")
	PORT_BIT(0x10, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_CODE(KEYCODE_W) PORT_NAME("Black")
	PORT_BIT(0x20, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_CODE(KEYCODE_E) PORT_NAME("Move")
	PORT_BIT(0x40, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_CODE(KEYCODE_R) PORT_NAME("Level")
	PORT_BIT(0x80, IP_ACTIVE_HIGH, IPT_KEYPAD) PORT_CODE(KEYCODE_T) PORT_NAME("Sound")
INPUT_PORTS_END



/******************************************************************************
    Machine Drivers
******************************************************************************/

void master_state::master(machine_config &config)
{
	/* basic machine hardware */
	Z80(config, m_maincpu, 8_MHz_XTAL/2);
	m_maincpu->set_addrmap(AS_PROGRAM, &master_state::main_trampoline);
	ADDRESS_MAP_BANK(config, "mainmap").set_map(&master_state::main_map).set_options(ENDIANNESS_LITTLE, 8, 16);

	const attotime irq_period = attotime::from_hz(429); // theoretical frequency from 555 timer (22nF, 150K, 1K5), measurement was 418Hz
	TIMER(config, m_irq_on).configure_periodic(FUNC(master_state::irq_on<INPUT_LINE_IRQ0>), irq_period);
	m_irq_on->set_start_delay(irq_period - attotime::from_nsec(22870)); // active for 22.87us
	TIMER(config, "irq_off").configure_periodic(FUNC(master_state::irq_off<INPUT_LINE_IRQ0>), irq_period);

	TIMER(config, "display_decay").configure_periodic(FUNC(ckingbase_state::display_decay_tick), attotime::from_msec(1));
	config.set_default_layout(layout_ck_master);

	/* sound hardware */
	SPEAKER(config, "speaker").front_center();
	DAC_2BIT_BINARY_WEIGHTED_ONES_COMPLEMENT(config, m_dac, 0).add_route(ALL_OUTPUTS, "speaker", 0.25);
	voltage_regulator_device &vref(VOLTAGE_REGULATOR(config, "vref"));
	vref.set_output(5.0);
	vref.add_route(0, "dac", 1.0, DAC_VREF_POS_INPUT);
	vref.add_route(0, "dac", -1.0, DAC_VREF_NEG_INPUT);
}



/******************************************************************************
    ROM Definitions
******************************************************************************/

ROM_START( ckmaster )
	ROM_REGION( 0x10000, "maincpu", 0 )
	ROM_LOAD("ckmaster.ic2", 0x0000, 0x2000, CRC(59cbec9e) SHA1(2e0629e65778da62bed857406b91a334698d2fe8) ) // D2764C, no label
ROM_END

} // anonymous namespace

/******************************************************************************
    Drivers
******************************************************************************/

/*    YEAR  NAME      PARENT  COMPAT  MACHINE  INPUT   CLASS        INIT         COMPANY       FULLNAME               FLAGS */
CONS( 1984, ckmaster, 0,      0,      master,  master, master_state, init_master, "Chess King", "Master (Chess King)", MACHINE_SUPPORTS_SAVE | MACHINE_CLICKABLE_ARTWORK | MACHINE_IMPERFECT_CONTROLS )
