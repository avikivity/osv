#include <algorithm>
#include <list>
#include <map>

#include "sched.hh"
#include "drivers/pci-function.hh"
#include "exceptions.hh"
#include "interrupt.hh"
#include "apic.hh"
#include <osv/trace.hh>

tracepoint<1001, unsigned, unsigned> trace_msix_migrate("msix_migrate", "vector=0x%02x apic_id=0x%x");

using namespace pci;

msix_vector::msix_vector(pci::function* dev)
    : _dev(dev)
{
    _vector = idt.register_handler([this] { interrupt(); });
}

msix_vector::~msix_vector()
{
    idt.unregister_handler(_vector);
}

pci::function* msix_vector::get_pci_function(void)
{
    return (_dev);
}

unsigned msix_vector::get_vector(void)
{
    return (_vector);
}

void msix_vector::msix_unmask_entries(void)
{
    for (auto entry_id : _entryids) {
        _dev->msix_unmask_entry(entry_id);
    }
}

void msix_vector::msix_mask_entries(void)
{
    for (auto entry_id : _entryids) {
        _dev->msix_mask_entry(entry_id);
    }
}

void msix_vector::set_handler(std::function<void ()> handler)
{
    _handler = handler;
}

void msix_vector::add_entryid(unsigned entry_id)
{
    _entryids.push_back(entry_id);
}

void msix_vector::interrupt(void)
{
    _handler();
}

void msix_vector::set_affinity(unsigned apic_id)
{
    msi_message msix_msg = apic->compose_msix(_vector, apic_id);
    for (auto entry_id : _entryids) {
        _dev->msix_write_entry(entry_id, msix_msg._addr, msix_msg._data);
    }
}

msix_wake_thread_with_affinity::msix_wake_thread_with_affinity(msix_vector& msix, sched::thread* thread)
    : _msix(msix)
    , _thread(thread)
    , _current()
    , _counter()
    , _cpu_stats(sched::cpus.size())  // FIXME: resize on cpu  hotplug
{
}

void msix_wake_thread_with_affinity::operator()()
{
    auto cpu = _thread->wake_get_cpu();
    ++_cpu_stats[cpu->id];
    if (++_counter == 1000) {  // FIXME: what's a good value?
        _counter = 0;
        auto imax = std::max_element(_cpu_stats.begin(), _cpu_stats.end());
        auto max = sched::cpus[imax - _cpu_stats.begin()];
        if (max != _current) {
            _current = max;
            trace_msix_migrate(_msix.get_vector(), max->arch.apic_id);
            _msix.set_affinity(max->arch.apic_id);
            std::fill(_cpu_stats.begin(), _cpu_stats.end(), 0);
        }
    }
}

interrupt_manager::interrupt_manager(pci::function* dev)
    : _dev(dev)
{
}

interrupt_manager::~interrupt_manager()
{

}

bool interrupt_manager::easy_register(std::initializer_list<msix_binding> bindings)
{
    unsigned n = bindings.size();

    std::vector<msix_vector*> assigned = request_vectors(n);

    if (assigned.size() != n) {
        free_vectors(assigned);
        return (false);
    }

    // Enable the device msix capability,
    // masks all interrupts...
    _dev->msix_enable();

    int idx=0;

    for (auto binding : bindings) {
        msix_vector* vec = assigned[idx++];
        auto isr = binding.isr;
        auto t = binding.t;

        msix_wake_thread_with_affinity wake(*vec, t);
        bool assign_ok = assign_isr(vec, [=]() mutable { if (isr) isr(); wake(); });
        if (!assign_ok) {
            free_vectors(assigned);
            return false;
        }
        bool setup_ok = setup_entry(binding.entry, vec);
        if (!setup_ok) {
            free_vectors(assigned);
            return false;
        }
    }

    // Save reference for assigned vectors
    _easy_vectors = assigned;
    unmask_interrupts(assigned);

    return (true);
}

void interrupt_manager::easy_unregister()
{
    free_vectors(_easy_vectors);
    _easy_vectors.clear();
}

std::vector<msix_vector*> interrupt_manager::request_vectors(unsigned num_vectors)
{
    std::vector<msix_vector*> results;

    auto num = std::min(num_vectors, _dev->msix_get_num_entries());

    for (unsigned i = 0; i < num; ++i) {
        results.push_back(new msix_vector(_dev));
    }

    return (results);
}

bool interrupt_manager::assign_isr(msix_vector* vector, std::function<void ()> handler)
{
    vector->set_handler(handler);

    return (true);
}

bool interrupt_manager::setup_entry(unsigned entry_id, msix_vector* msix)
{
    auto vector = msix->get_vector();
    msi_message msix_msg = apic->compose_msix(vector, 0);

    if (msix_msg._addr == 0) {
        return (false);
    }

    if (!_dev->msix_write_entry(entry_id, msix_msg._addr, msix_msg._data)) {
        return (false);
    }

    msix->add_entryid(entry_id);
    return (true);
}

void interrupt_manager::free_vectors(const std::vector<msix_vector*>& vectors)
{
    for (auto msix : vectors) {
        delete msix;
    }
}

bool interrupt_manager::unmask_interrupts(const std::vector<msix_vector*>& vectors)
{
    for (auto msix : vectors) {
        msix->msix_unmask_entries();
    }

    return (true);
}

inter_processor_interrupt::inter_processor_interrupt(std::function<void ()> handler)
    : _vector(idt.register_handler(handler))
{
}

inter_processor_interrupt::~inter_processor_interrupt()
{
    idt.unregister_handler(_vector);
}

void inter_processor_interrupt::send(sched::cpu* cpu)
{
    apic->ipi(cpu->arch.apic_id, _vector);
}

void inter_processor_interrupt::send_allbutself(){
    apic->ipi_allbutself(_vector);
}
