import { useId } from 'react';
import { Modal, type ModalProps } from '@rakuensoftware/smoothgui';

/** Label the vendor dialog with its visible title. SmoothGUI's Modal accepts a
 * title node but does not yet forward ARIA attributes to its dialog element. */
export default function TitledModal({ title, ...props }: ModalProps) {
  const titleId = useId();
  return <Modal {...props} title={title === undefined ? undefined : <span id={titleId} ref={node => {
    node?.closest('[role="dialog"]')?.setAttribute('aria-labelledby', titleId);
  }}>{title}</span>} />;
}
